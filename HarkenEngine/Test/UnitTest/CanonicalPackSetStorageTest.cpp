#include "Common/ObjectIdTestSupport.h"
#include "harken/media/PackTable.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <new>
#include <limits>
#include <type_traits>
#include <utility>

#include "UnitTest/AllocationProbe.h"

namespace {

static_assert(std::is_nothrow_destructible_v<harken::PackTable::PreparedSetMutation>);

struct SetStorageMember {
    harken::PackTable::AlignedBuffer bytes;
    std::vector<harken::PackTocEntry> media;
    harken::PackRevisionKey key{};

    SetStorageMember(std::uint64_t packId, VnSoundId soundId, float value,
                     std::size_t frames = 8) {
        const auto byteCount = std::max(std::size_t{64}, frames * sizeof(float));
        bytes.reset(static_cast<std::uint8_t*>(::operator new[](
            byteCount, std::align_val_t{harken::kPackBlobAlign})));
        std::memset(bytes.get(), 0, byteCount);
        auto* samples = reinterpret_cast<float*>(bytes.get());
        std::fill(samples, samples + frames, value);
        harken::PackTocEntry row{};
        row.soundId = soundId;
        row.sampleRate = 48000;
        row.channelCount = 1;
        row.frameCount = frames;
        row.blobChecksum = harken::Fnv1a64(bytes.get(), frames * sizeof(float));
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

TEST(CanonicalPackSetStorage, CompactMediaSharesExactBitsAndDistinguishesSignedZero) {
    const auto frames = 16 * harken::detail::StablePageReservation::SystemPageBytes() /
                        sizeof(std::int16_t) + 1;
    for (const int path : {0, 1, 2}) {
        SCOPED_TRACE(path);
        harken::PCMRegistry registry;
        harken::PackTable packs(3, 4);
        const auto sound = harkentest::ObjectId(100);
        SetStorageMember a(21, sound, 0.0f, frames), b(22, sound, 0.0f, frames),
                         negative(23, sound, -0.0f, frames);
        const auto load = [&](SetStorageMember& member, VnPackHandle* handle) {
            if (path != 0) {
                std::vector<harken::PCMRegistry::PreparedPackSamples> samples(1);
                auto input = member.Input();
                const void* preparedAddress = nullptr;
                if (path == 2) {
                    const float* channel = reinterpret_cast<const float*>(member.bytes.get());
                    VnPCMDesc desc{};
                    desc.structSize = sizeof(desc);
                    desc.sampleRate = 48000;
                    desc.channels = 1;
                    desc.frames = frames;
                    desc.layout = VN_PCM_LAYOUT_PLANAR;
                    desc.policy = VN_PCM_POLICY_COPY;
                    desc.samplesPlanar = &channel;
                    const auto result = registry.PreparePackSamples(sound, &desc, &samples[0]);
                    if (result != VN_OK) return result;
                    preparedAddress = samples[0].View().channels[0];
                    input.bytes = nullptr;
                    input.samples = &samples;
                }
                harken::PackTable::PreparedSetMutation prepared;
                const bool alreadyRegistered = registry.CookieOf(sound) != 0;
                const auto result = packs.PrepareCanonicalSet({input}, {}, registry, &prepared);
                if (result == VN_OK) {
                    *handle = prepared.HandleAt(0);
                    packs.CommitPreparedSet(&prepared, registry);
                    if (path == 2) {
                        EXPECT_EQ(alreadyRegistered, samples[0].IsPrepared());
                        if (!alreadyRegistered)
                            EXPECT_EQ(preparedAddress, registry.Lookup(sound).channels[0]);
                    }
                }
                return result;
            }
            harken::PackTable::PreparedLoadToken prepared;
            const auto result = packs.PrepareLoad(&prepared);
            if (result != VN_OK) return result;
            member.key.handle = prepared.Handle();
            member.key.slotGeneration = static_cast<std::uint32_t>(prepared.Handle() >> 32);
            std::vector<harken::PackEntry> entries(1);
            entries[0].soundId = sound;
            entries[0].frameCount = frames;
            return packs.LoadPrepared(&prepared, member.bytes, std::move(entries),
                                      member.media, registry, handle, &member.key);
        };
        VnPackHandle handles[3]{};
        ASSERT_EQ(VN_OK, load(a, &handles[0]));
        const auto original = registry.Lookup(sound);
        const auto cookie = registry.CookieOf(sound);
        ASSERT_EQ(harken::PcmSampleFormat::ExactInt16, original.sampleFormat);
        ASSERT_EQ(VN_OK, load(b, &handles[1]));
        EXPECT_EQ(cookie, registry.CookieOf(sound));
        EXPECT_EQ(original.channels[0], registry.Lookup(sound).channels[0]);
        EXPECT_EQ(VN_IN_USE, load(negative, &handles[2]));
        EXPECT_EQ(0u, handles[2]);
        EXPECT_EQ(cookie, registry.CookieOf(sound));
        ASSERT_EQ(VN_OK, packs.Unload(handles[0], registry));
        EXPECT_EQ(original.channels[0], registry.Lookup(sound).channels[0]);
        ASSERT_EQ(VN_OK, packs.Unload(handles[1], registry));
        ASSERT_EQ(VN_OK, load(negative, &handles[2]));
        const auto replaced = registry.Lookup(sound);
        EXPECT_EQ(harken::PcmSampleFormat::Float32, replaced.sampleFormat);
        EXPECT_EQ(original.contentRevision, replaced.contentRevision);
        const float expected = -0.0f;
        const float actual = replaced.ReadSample(0, frames - 1);
        EXPECT_EQ(0, std::memcmp(&expected, &actual, sizeof(float)));
        EXPECT_EQ(VN_OK, packs.Unload(handles[2], registry));
        EXPECT_EQ(0u, registry.Count());
    }
}

TEST(CanonicalPackSetStorage, PreparedSamplesKeepIdentityAcrossAdoptionAndLateRefusal) {
    harken::PCMRegistry registry;
    const auto first = harkentest::ObjectId(100), second = harkentest::ObjectId(101);
    const float left[]{0.25f, -0.0f, 0.75f}, right[]{-0.5f, 0.0f, 0.125f};
    const float* planes[]{left, right};
    VnPCMDesc desc{};
    desc.structSize = sizeof(desc);
    desc.sampleRate = 48000;
    desc.channels = 2;
    desc.frames = 3;
    desc.layout = VN_PCM_LAYOUT_PLANAR;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.samplesPlanar = planes;
    harken::PCMRegistry::PreparedPackSamples samples;
    ASSERT_EQ(VN_OK, registry.PreparePackSamples(first, &desc, &samples, {1, 3}));
    const auto view = samples.View();
    EXPECT_EQ(VN_IN_USE, registry.PreparePackSamples(second, &desc, &samples));
    harken::PCMRegistry::PackReferenceRegistration registrations[2]{};
    registrations[0].soundId = first;
    registrations[0].samples = &samples;
    registrations[1].soundId = second;
    harken::PCMRegistry::PreparedPackMutation refused;
    EXPECT_EQ(VN_INVALID_ARG, registry.PreparePackMutation(registrations, 2, 0, &refused));
    EXPECT_FALSE(samples.IsPrepared());
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_EQ(0u, registry.Count());

    ASSERT_EQ(VN_OK, registry.PreparePackSamples(first, &desc, &samples, {1, 3}));
    const auto adopted = samples.View();
    harken::PCMRegistry::PreparedPackMutation prepared;
    ASSERT_EQ(VN_OK, registry.PreparePackMutation(registrations, 1, 0, &prepared));
    const auto held = prepared.ReferenceOf(first);
    EXPECT_EQ(1u, held.Cookie());
    EXPECT_EQ(adopted.channels[0], held.View().channels[0]);
    EXPECT_EQ(adopted.useCount, held.View().useCount);
    EXPECT_EQ(view.contentRevision, held.View().contentRevision);
    registry.CommitPreparedPackMutation(&prepared);
    EXPECT_EQ(adopted.channels[1], registry.Lookup(first).channels[1]);
    EXPECT_EQ(1u, registry.Lookup(first).loop.startFrame);
    EXPECT_EQ(3u, registry.Lookup(first).loop.endFrame);
}

TEST(CanonicalPackSetStorage, WindowedSamplesKeepPhysicalBitsAndFrameMajorRevision) {
    const auto frames = 16 * harken::detail::StablePageReservation::SystemPageBytes() /
                        sizeof(std::int16_t) + 3;
    for (const auto layout : {VN_PCM_LAYOUT_PLANAR, VN_PCM_LAYOUT_INTERLEAVED}) {
        for (const float last : {0.25f, -0.0f, std::numeric_limits<float>::denorm_min(), 0.1f, 1.0f}) {
            SCOPED_TRACE(layout);
            SCOPED_TRACE(last);
            std::vector<float> physical(frames * 2);
            for (std::size_t i = 0; i < physical.size(); ++i)
                physical[i] = static_cast<float>(static_cast<int>(i % 32768) - 16384) * 0x1p-15f;
            physical.back() = last;
            const float* planes[]{physical.data(), physical.data() + frames};
            VnPCMDesc shape{};
            shape.structSize = sizeof(shape);
            shape.sampleRate = 44100;
            shape.channels = 2;
            shape.frames = frames;
            shape.layout = layout;
            shape.policy = VN_PCM_POLICY_COPY;
            harken::PCMRegistry registry;
            const auto sound = harkentest::ObjectId(100);
            harken::PCMRegistry::PackSampleBuilder builder;
            ASSERT_EQ(VN_OK, registry.BeginPackSamples(sound, &shape, &builder, {7, frames - 3}));
            for (std::size_t i = 0; i < physical.size();) {
                const auto count = std::min(std::size_t{16128}, physical.size() - i);
                ASSERT_EQ(VN_OK, builder.Append(physical.data() + i, count));
                i += count;
            }
            harken::PCMRegistry::PreparedPackSamples prepared;
            ASSERT_EQ(VN_OK, builder.Complete(&prepared));
            shape.samplesInterleaved = physical.data();
            shape.samplesPlanar = planes;
            shape.policy = VN_PCM_POLICY_REFERENCE;
            ASSERT_EQ(VN_OK, registry.RegisterPCM(sound, &shape, nullptr, {7, frames - 3}));
            const auto expected = registry.Lookup(sound);
            const auto actual = prepared.View();
            EXPECT_EQ(expected.contentRevision, actual.contentRevision);
            EXPECT_EQ(last == 0.25f ? harken::PcmSampleFormat::ExactInt16 :
                      harken::PcmSampleFormat::Float32, actual.sampleFormat);
            for (std::uint32_t ch = 0; ch < 2; ++ch) {
                for (std::uint64_t frame = 0; frame < frames; ++frame) {
                    const float a = actual.ReadSample(ch, frame), b = expected.ReadSample(ch, frame);
                    ASSERT_EQ(0, std::memcmp(&a, &b, sizeof(float)));
                }
            }
        }
    }
}

TEST(CanonicalPackSetStorage, WindowedSamplePairsKeepEachCanonicalRevisionAndFinalOwner) {
    const auto frames = 16 * harken::detail::StablePageReservation::SystemPageBytes() /
                        sizeof(std::int16_t) + 3;
    for (const auto layout : {VN_PCM_LAYOUT_PLANAR, VN_PCM_LAYOUT_INTERLEAVED}) {
        for (const auto channels : {1u, 2u, harken::kMaxChannels}) {
            for (unsigned formats = 0; formats < 4; ++formats) {
                SCOPED_TRACE(layout);
                SCOPED_TRACE(channels);
                SCOPED_TRACE(formats);
                harken::PCMRegistry registry;
                std::vector<float> physical[2];
                const float* planes[2][harken::kMaxChannels]{};
                VnPCMDesc shape[2]{};
                harken::PCMRegistry::PackSampleBuilder builders[2];
                harken::PCMRegistry::PreparedPackSamples prepared[2];
                for (unsigned side = 0; side < 2; ++side) {
                    physical[side].resize(frames * channels);
                    for (std::size_t i = 0; i < physical[side].size(); ++i)
                        physical[side][i] = static_cast<float>(
                            static_cast<int>((i * (side + 1) + side) % 32768) - 16384) * 0x1p-15f;
                    if ((formats & (1u << side)) != 0)
                        physical[side].back() = side == 0 ? -0.0f : std::numeric_limits<float>::denorm_min();
                    for (unsigned ch = 0; ch < channels; ++ch)
                        planes[side][ch] = physical[side].data() + ch * frames;
                    auto& desc = shape[side];
                    desc.structSize = sizeof(desc);
                    desc.sampleRate = side == 0 ? 44100 : 48000;
                    desc.channels = channels;
                    desc.frames = frames;
                    desc.layout = layout;
                    desc.policy = VN_PCM_POLICY_COPY;
                    ASSERT_EQ(VN_OK, registry.BeginPackSamples(harkentest::ObjectId(100 + side),
                        &desc, &builders[side], {3 + side, frames - 2 + side}));
                    const auto count = physical[side].size() - (side == 1 ? 1 : 0);
                    ASSERT_EQ(VN_OK, builders[side].Append(physical[side].data(), count));
                }
                VnResult results[2]{VN_IN_USE, VN_BAD_PACK};
                EXPECT_FALSE(builders[0].TryCompletePair(builders[1], &prepared[0], &prepared[1], results));
                EXPECT_FALSE(prepared[0].IsPrepared());
                EXPECT_FALSE(prepared[1].IsPrepared());
                EXPECT_EQ(VN_IN_USE, results[0]);
                EXPECT_EQ(VN_BAD_PACK, results[1]);
                ASSERT_EQ(VN_OK, builders[1].Append(&physical[1].back(), 1));
                ASSERT_TRUE(builders[0].TryCompletePair(builders[1], &prepared[0], &prepared[1], results));
                ASSERT_EQ(VN_OK, results[0]);
                ASSERT_EQ(VN_OK, results[1]);
                for (unsigned side = 0; side < 2; ++side) {
                    shape[side].samplesPlanar = planes[side];
                    shape[side].samplesInterleaved = physical[side].data();
                    shape[side].policy = VN_PCM_POLICY_REFERENCE;
                    const auto id = harkentest::ObjectId(100 + side);
                    ASSERT_EQ(VN_OK, registry.RegisterPCM(id, &shape[side], nullptr,
                        {3 + side, frames - 2 + side}));
                    const auto actual = prepared[side].View(), expected = registry.Lookup(id);
                    EXPECT_EQ(expected.contentRevision, actual.contentRevision);
                    EXPECT_EQ((formats & (1u << side)) == 0 ? harken::PcmSampleFormat::ExactInt16 :
                        harken::PcmSampleFormat::Float32, actual.sampleFormat);
                    for (unsigned ch = 0; ch < channels; ++ch) {
                        for (std::uint64_t frame = 0; frame < frames; ++frame) {
                            const float a = actual.ReadSample(ch, frame), b = expected.ReadSample(ch, frame);
                            ASSERT_EQ(0, std::memcmp(&a, &b, sizeof(float)));
                        }
                    }
                }
                const auto firstPointer = prepared[0].View().channels[0];
                EXPECT_FALSE(builders[0].TryCompletePair(builders[1], &prepared[0], &prepared[1], results));
                EXPECT_EQ(firstPointer, prepared[0].View().channels[0]);
            }
        }
    }
}

TEST(CanonicalPackSetStorage, WindowedDuplicateSharesOnlyExactUnpublishedSamples) {
    const auto frames = 16 * harken::detail::StablePageReservation::SystemPageBytes() /
                        sizeof(std::int16_t) + 3;
    std::vector<float> physical(frames, 0.25f);
    harken::PCMRegistry registry;
    const auto sound = harkentest::ObjectId(100);
    VnPCMDesc shape{};
    shape.structSize = sizeof(shape);
    shape.sampleRate = 48000;
    shape.channels = 1;
    shape.frames = frames;
    shape.layout = VN_PCM_LAYOUT_PLANAR;
    shape.policy = VN_PCM_POLICY_COPY;
    harken::PCMRegistry::PackSampleBuilder first;
    ASSERT_EQ(VN_OK, registry.BeginPackSamples(sound, &shape, &first));
    ASSERT_EQ(VN_OK, first.Append(physical.data(), frames));
    harken::PCMRegistry::PreparedPackSamples source;
    ASSERT_EQ(VN_OK, first.Complete(&source));
    for (const bool equal : {true, false}) {
        harken::PCMRegistry::PackSampleBuilder duplicate;
        ASSERT_EQ(VN_OK, registry.BeginPackSamples(sound, &shape, &duplicate, {}, &source));
        ASSERT_EQ(VN_OK, duplicate.Append(physical.data(), frames - 1));
        const float last = equal ? 0.25f : -0.0f;
        ASSERT_EQ(VN_OK, duplicate.Append(&last, 1));
        harken::PCMRegistry::PreparedPackSamples result;
        ASSERT_EQ(VN_OK, duplicate.Complete(&result));
        EXPECT_EQ(equal, result.View().channels[0] == source.View().channels[0]);
        EXPECT_EQ(0.25f, result.View().ReadSample(0, frames - 2));
        const float actual = result.View().ReadSample(0, frames - 1);
        EXPECT_EQ(0, std::memcmp(&last, &actual, sizeof(float)));
    }
    harken::detail::StablePageReservation::FailNextReservationForTests();
    harken::PCMRegistry::PackSampleBuilder refused;
    EXPECT_EQ(VN_BACKPRESSURE, registry.BeginPackSamples(sound, &shape, &refused));
    ASSERT_EQ(VN_OK, registry.BeginPackSamples(sound, &shape, &refused));
    refused.SetCommitPageLimitForTests(0);
    EXPECT_EQ(VN_BACKPRESSURE, refused.Append(physical.data(), 1));
    EXPECT_FALSE(refused.ReservationUnavailable());
    harken::PCMRegistry::PreparedPackSamples result;
    EXPECT_NE(VN_OK, refused.Complete(&result));
    EXPECT_FALSE(result.IsPrepared());

    harken::PCMRegistry::PackSampleBuilder promotion;
    ASSERT_EQ(VN_OK, registry.BeginPackSamples(sound, &shape, &promotion));
    ASSERT_EQ(VN_OK, promotion.Append(physical.data(), 1));
    promotion.SetCommitPageLimitForTests(0);
    const float negativeZero = -0.0f;
    EXPECT_EQ(VN_BACKPRESSURE, promotion.Append(&negativeZero, 1));
    EXPECT_FALSE(promotion.ReservationUnavailable());
    EXPECT_NE(VN_OK, promotion.Complete(&result));
    EXPECT_FALSE(result.IsPrepared());
    EXPECT_EQ(0u, registry.CookieOf(sound));
    EXPECT_EQ(0.25f, source.View().ReadSample(0, frames - 1));

    harken::PCMRegistry::PackSampleBuilder aliasBuilder;
    ASSERT_EQ(VN_OK, registry.BeginPackSamples(sound, &shape, &aliasBuilder, {}, &source));
    ASSERT_EQ(VN_OK, aliasBuilder.Append(physical.data(), frames));
    harken::PCMRegistry::PreparedPackSamples alias;
    ASSERT_EQ(VN_OK, aliasBuilder.Complete(&alias));
    harken::PCMRegistry::PackReferenceRegistration registration{};
    registration.soundId = sound;
    registration.samples = &source;
    harken::PCMRegistry::PreparedPackMutation prepared;
    ASSERT_EQ(VN_OK, registry.PreparePackMutation(&registration, 1, 0, &prepared));
    const auto held = prepared.ReferenceOf(sound);
    const auto cookie = held.Cookie();
    registry.CommitPreparedPackMutation(&prepared);
    registration.samples = &alias;
    harken::PCMRegistry::PreparedPackMutation repeated;
    EXPECT_EQ(VN_INVALID_ARG, registry.PreparePackMutation(&registration, 1, 0, &repeated));
    EXPECT_FALSE(repeated.IsPrepared());
    EXPECT_TRUE(alias.IsPrepared());
    EXPECT_EQ(cookie, held.Cookie());
    EXPECT_EQ(cookie, registry.CookieOf(sound));
    EXPECT_EQ(held.View().channels[0], registry.Lookup(sound).channels[0]);
    EXPECT_EQ(held.View().useCount, registry.Lookup(sound).useCount);
    EXPECT_EQ(1u, registry.Count());
}

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

TEST(CanonicalPackSetStorage, EqualMediaRevisionsDoNotPinOldRowsForCurrentReaders) {
    harken::PCMRegistry registry;
    harken::PackTable packs(1, 1, 1);
    const auto sound = harkentest::ObjectId(100);
    SetStorageMember member(21, sound, 0.25f);
    harken::PackTable::PreparedSetMutation initial;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({member.Input()}, {}, registry, &initial));
    packs.CommitPreparedSet(&initial, registry);
    const auto handle = initial.HandleAt(0);
    const auto current = registry.Lookup(sound);
    ASSERT_NE(nullptr, current.useCount);
    current.useCount->fetch_add(1, std::memory_order_acq_rel);
    for (unsigned revision = 2; revision <= 32; ++revision) {
        SCOPED_TRACE(revision);
        member.key.contentRevision = revision;
        harken::PackTable::PreparedSetMutation next;
        const auto result = packs.PrepareCanonicalSet({member.Input(handle)}, {}, registry, &next);
        EXPECT_EQ(VN_OK, result);
        if (result != VN_OK) break;
        packs.CommitPreparedSet(&next, registry);
        packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
        EXPECT_EQ(0u, packs.RetiringCount());
        EXPECT_EQ(current.useCount, registry.Lookup(sound).useCount);
        EXPECT_FLOAT_EQ(0.25f, current.ReadSample(0, 0));
        if (packs.RetiringCount() != 0u) break;
    }
    current.useCount->fetch_sub(1, std::memory_order_acq_rel);
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    ASSERT_EQ(VN_OK, packs.Unload(handle, registry));
    EXPECT_EQ(0u, packs.RetiringCount());
    EXPECT_EQ(0u, packs.LoadedCount());
    EXPECT_EQ(0u, registry.Count());
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
    EXPECT_FLOAT_EQ(0.25f, old.ReadSample(0, 0));

    SetStorageMember changed(21, sound, 0.75f);
    harken::PackTable::PreparedSetMutation replacement;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({changed.Input(handleA)}, {}, registry, &replacement));
    packs.CommitPreparedSet(&replacement, registry);
    EXPECT_NE(tokenA, replacement.ResourceTokenAt(0));
    EXPECT_FLOAT_EQ(0.75f, registry.Lookup(sound).ReadSample(0, 0));
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    std::uint64_t checksum = 0;
    bool known = false;
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(reader, sound, registry, &old, &checksum, &known));
    EXPECT_FLOAT_EQ(0.25f, old.ReadSample(0, 0));
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
    EXPECT_FLOAT_EQ(0.75f, registry.Lookup(soundA).ReadSample(0, 0));
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    harken::PCMView oldA{}, oldB{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(reader, soundA, registry, &oldA));
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(reader, soundB, registry, &oldB));
    EXPECT_FLOAT_EQ(0.25f, oldA.ReadSample(0, 0));
    EXPECT_FLOAT_EQ(0.5f, oldB.ReadSample(0, 0));
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
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).ReadSample(0, 0));
    packs.SetStorageCommitPageLimitsForTests(0, 0, SIZE_MAX);
    harken::PackTable::PreparedSetMutation partial;
    EXPECT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({changedA.Input(handleA)}, {}, registry, &partial));
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).ReadSample(0, 0));
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet(
        {changedA.Input(handleA), changedB.Input(handleB)}, {}, registry, &next));
    packs.SetStorageCommitPageLimitsForTests(0, 0, 0);
    EXPECT_EQ(handleA, next.HandleAt(0));
    EXPECT_EQ(handleB, next.HandleAt(1));
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).ReadSample(0, 0));
    packs.CommitPreparedSet(&next, registry);
    EXPECT_FLOAT_EQ(0.75f, registry.Lookup(harkentest::ObjectId(100)).ReadSample(0, 0));
    harken::PCMView old{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(closure, harkentest::ObjectId(100), registry, &old));
    EXPECT_FLOAT_EQ(0.25f, old.ReadSample(0, 0));
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
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).ReadSample(0, 0));
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
    EXPECT_FLOAT_EQ(0.25f, old.ReadSample(0, 0));
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
    EXPECT_FLOAT_EQ(0.25f, old.ReadSample(0, 0));
    EXPECT_EQ(VN_OK, packs.ReleaseResourceClosure(closure));
}

TEST(CanonicalPackSetStorage, LoopViewAndContentIdentityMatchDirectRegistration) {
    for (const auto loop : {harken::PcmLoopRegion{0, 0},
                            harken::PcmLoopRegion{2, 6},
                            harken::PcmLoopRegion{2, 0}}) {
        harken::PCMRegistry registry, direct;
        harken::PackTable packs(2, 4);
        SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.25f);
        a.media[0].flags = b.media[0].flags = harken::kPackFlagLoopable;
        a.media[0].loopStartFrame = b.media[0].loopStartFrame = loop.startFrame;
        a.media[0].loopEndFrame = b.media[0].loopEndFrame = loop.endFrame;
        harken::PackTable::PreparedSetMutation first;
        ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &first));
        packs.CommitPreparedSet(&first, registry);
        const auto view = registry.Lookup(harkentest::ObjectId(100));
        EXPECT_EQ(a.media[0].loopStartFrame, view.loop.startFrame);
        EXPECT_EQ(loop.startFrame != 0 && loop.endFrame == 0 ? 8u : loop.endFrame,
                  view.loop.endFrame);
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
        harken::PackTable::PreparedSetMutation same;
        ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet(
            {a.Input(first.HandleAt(0)), b.Input(first.HandleAt(1))}, {}, registry, &same));
        packs.CommitPreparedSet(&same, registry);
        EXPECT_EQ(view.useCount, registry.Lookup(harkentest::ObjectId(100)).useCount);
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

TEST(CanonicalPackSetStorage, SinglePackEofLoopLoadsSharesAndReplaces) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4, 4);
    const auto sound = harkentest::ObjectId(100);
    const auto entriesFor = [](const SetStorageMember& member) {
        std::vector<harken::PackEntry> entries(1);
        const auto& media = member.media[0];
        entries[0].soundId = media.soundId;
        entries[0].flags = media.flags;
        entries[0].frameCount = media.frameCount;
        entries[0].loopStartFrame = media.loopStartFrame;
        entries[0].loopEndFrame = media.loopEndFrame;
        return entries;
    };
    SetStorageMember a(21, sound, 0.25f), b(22, sound, 0.25f), next(21, sound, 0.5f);
    for (auto* member : {&a, &b, &next}) {
        member->media[0].flags = harken::kPackFlagLoopable;
        member->media[0].loopStartFrame = 2;
        member->media[0].loopEndFrame = 0;
    }
    VnPackHandle handles[2]{};
    unsigned index = 0;
    const auto prepareSamples = [&](SetStorageMember& member,
        std::vector<harken::PCMRegistry::PreparedPackSamples>* prepared) {
        const float* plane = reinterpret_cast<const float*>(member.bytes.get());
        VnPCMDesc desc{};
        desc.structSize = sizeof(desc);
        desc.sampleRate = member.media[0].sampleRate;
        desc.channels = 1;
        desc.frames = member.media[0].frameCount;
        desc.layout = VN_PCM_LAYOUT_PLANAR;
        desc.policy = VN_PCM_POLICY_COPY;
        desc.samplesPlanar = &plane;
        prepared->resize(1);
        const auto result = registry.PreparePackSamples(sound, &desc, &prepared->front(),
            {member.media[0].loopStartFrame, member.media[0].frameCount});
        if (result == VN_OK) member.bytes.reset();
        return result;
    };
    for (auto* member : {&a, &b}) {
        harken::PackTable::PreparedLoadToken load;
        ASSERT_EQ(VN_OK, packs.PrepareLoad(&load));
        member->key.handle = load.Handle();
        member->key.slotGeneration = static_cast<std::uint32_t>(load.Handle() >> 32);
        std::vector<harken::PCMRegistry::PreparedPackSamples> prepared;
        const bool adopt = member == &a;
        if (adopt) ASSERT_EQ(VN_OK, prepareSamples(*member, &prepared));
        const auto view = adopt ? prepared.front().View() : harken::PCMView{};
        ASSERT_EQ(VN_OK, packs.LoadPrepared(&load, member->bytes, entriesFor(*member),
            member->media, registry, &handles[index++], &member->key, adopt ? &prepared : nullptr));
        if (adopt) {
            EXPECT_EQ(view.channels[0], registry.Lookup(sound).channels[0]);
            EXPECT_EQ(view.useCount, registry.Lookup(sound).useCount);
        }
    }
    EXPECT_EQ(packs.Entries(handles[0])->front().registrationCookie,
              packs.Entries(handles[1])->front().registrationCookie);
    ASSERT_EQ(VN_OK, packs.Unload(handles[1], registry));
    const auto old = registry.Lookup(sound);
    old.useCount->fetch_add(1, std::memory_order_acq_rel);
    next.key.handle = handles[0];
    next.key.slotGeneration = static_cast<std::uint32_t>(handles[0] >> 32);
    next.key.contentRevision = 2;
    auto entries = entriesFor(next);
    std::vector<harken::PCMRegistry::PreparedPackSamples> prepared;
    ASSERT_EQ(VN_OK, prepareSamples(next, &prepared));
    const auto nextView = prepared.front().View();
    harken::PackTable::PreparedMutation replacement;
    ASSERT_EQ(VN_OK, packs.PrepareRevisionMutation(handles[0], next.bytes, entries,
        next.media, registry, &replacement, &next.key, &prepared));
    packs.CommitPreparedMutation(&replacement, registry);
    EXPECT_EQ(nextView.channels[0], registry.Lookup(sound).channels[0]);
    EXPECT_EQ(nextView.useCount, registry.Lookup(sound).useCount);
    EXPECT_EQ(2u, registry.Lookup(sound).loop.startFrame);
    EXPECT_EQ(8u, registry.Lookup(sound).LoopEndFrame());
    EXPECT_FLOAT_EQ(0.5f, registry.Lookup(sound).ReadSample(0, 0));
    EXPECT_FLOAT_EQ(0.25f, old.ReadSample(0, 0));
    old.useCount->fetch_sub(1, std::memory_order_acq_rel);
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    ASSERT_EQ(VN_OK, packs.Unload(handles[0], registry));
    EXPECT_EQ(0u, packs.LoadedCount());
    EXPECT_EQ(0u, packs.RetiringCount());
    EXPECT_EQ(0u, registry.Count());
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
    a.media[0].loopStartFrame = 2;
    a.media[0].loopEndFrame = 0;
    harken::PackTable::PreparedSetMutation next;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(first.HandleAt(0))}, {}, registry, &next));
    packs.CommitPreparedSet(&next, registry);
    EXPECT_EQ(2u, registry.Lookup(harkentest::ObjectId(100)).loop.startFrame);
    EXPECT_EQ(8u, registry.Lookup(harkentest::ObjectId(100)).loop.endFrame);
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
    EXPECT_FLOAT_EQ(0.25f, old.ReadSample(0, 0));
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
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).ReadSample(0, 0));
    harken::PackTable::PreparedSetMutation retry;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({b.Input(first.HandleAt(0))}, {}, registry, &retry));
    EXPECT_EQ(2u, retry.ResourceTokenAt(0));
    packs.CommitPreparedSet(&retry, registry);
    EXPECT_FLOAT_EQ(0.75f, registry.Lookup(harkentest::ObjectId(100)).ReadSample(0, 0));
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

TEST(CanonicalPackSetStorage, EqualLegacyReferenceMediaCannotAcquireAnotherPackOwner) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4);
    const auto sound = harkentest::ObjectId(100);
    SetStorageMember legacy(21, sound, 0.25f), shared(22, sound, 0.25f);
    std::vector<harken::PackEntry> entries(1);
    entries[0].soundId = sound;
    entries[0].frameCount = 8;
    harken::PackTable::PreparedLoadToken load;
    ASSERT_EQ(VN_OK, packs.PrepareLoad(&load));
    VnPackHandle handle = 0;
    ASSERT_EQ(VN_OK, packs.LoadPrepared(&load, legacy.bytes, std::move(entries),
                                      legacy.media, registry, &handle));
    const auto cookie = registry.CookieOf(sound);
    harken::PackTable::PreparedSetMutation set;
    EXPECT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({shared.Input()}, {}, registry, &set));
    ASSERT_EQ(VN_OK, packs.PrepareLoad(&load));
    entries.resize(1);
    entries[0].soundId = sound;
    entries[0].frameCount = 8;
    shared.key.handle = load.Handle();
    shared.key.slotGeneration = static_cast<std::uint32_t>(load.Handle() >> 32);
    VnPackHandle refusedHandle = 0;
    EXPECT_EQ(VN_IN_USE, packs.LoadPrepared(&load, shared.bytes, std::move(entries),
                                          shared.media, registry, &refusedHandle, &shared.key));
    EXPECT_EQ(0u, refusedHandle);
    EXPECT_EQ(cookie, registry.CookieOf(sound));
    EXPECT_EQ(1u, packs.LoadedCount());
    ASSERT_EQ(VN_OK, packs.Unload(handle, registry));
    EXPECT_EQ(0u, registry.Count());
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
