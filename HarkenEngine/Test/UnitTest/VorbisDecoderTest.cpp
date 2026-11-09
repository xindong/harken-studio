#include "Common/VorbisFixture.h"
#include "../../ThirdParty/vorbis-decoder/decoder.h"
#include "harken/core/AllocGuard.h"
#include "harken/core/StreamRing.h"
#include "harken/media/VorbisDecoder.h"
#include "AllocationProbe.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
struct Memory {
    std::size_t allocations = 0, live = 0, failAt = 0;
    HvMemory Callbacks() {
        return {this, [](void* user, std::size_t n) -> void* {
            auto& m = *static_cast<Memory*>(user);
            if (++m.allocations == m.failAt) return nullptr;
            auto* p = std::malloc(n); if (p) ++m.live; return p;
        }, [](void* user, void* p) {
            if (p) { --static_cast<Memory*>(user)->live; std::free(p); }
        }};
    }
};
using Tables = std::unique_ptr<HvTables, decltype(&hv_tables_destroy)>;
using DecoderSetup = std::unique_ptr<HvSetup, decltype(&hv_setup_destroy)>;
using Backing = std::vector<std::max_align_t>;
Backing Storage(std::size_t bytes) { return Backing((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t)); }
constexpr std::size_t kSetupBudget = 64 * 1024 * 1024;

TEST(VorbisDecoder, FloorZeroIsExplicitlyUnsupportedInResourceAndFixturePreparation) {
    const auto fixture = harken::test::MakeVorbisFixture(1);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    std::vector<std::uint8_t> setup{5, 'v', 'o', 'r', 'b', 'i', 's'};
    unsigned bit = 0;
    auto put = [&](std::uint32_t value, unsigned n) {
        for (unsigned i = 0; i < n; ++i, ++bit) {
            if ((bit & 7) == 0) setup.push_back(0);
            setup.back() |= ((value >> i) & 1) << (bit & 7);
        }
    };
    put(0,8); put(0x564342,24); put(1,16); put(1,24);
    put(0,1); put(0,1); put(0,5); put(1,4);
    put(0,32); put(0,32); put(0,4); put(0,1); put(0,1);
    put(0,6); put(0,16);
    put(0,6); put(0,16); put(1,8); put(48000,16); put(63,16);
    put(6,6); put(100,8); put(0,4); put(0,8);
    put(0,6); put(0,16); put(0,24); put(0,24); put(0,24);
    put(0,6); put(0,8); put(0,3); put(0,1);
    put(0,6); put(0,16); put(0,1); put(0,1); put(0,2); put(0,8); put(0,8); put(0,8);
    put(0,6); put(0,1); put(0,16); put(0,16); put(0,8); put(1,1);
    try {
        harken::test::ValidateVorbisFixtureSetup(media.ident, media.identBytes, setup.data(), setup.size());
        FAIL() << "floor zero accepted";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "Unsupported Vorbis floor type");
    }
    Memory memory;
    Tables tables(hv_tables_create(memory.Callbacks(), kSetupBudget), hv_tables_destroy);
    HvResult result;
    DecoderSetup rejected(hv_setup_create(memory.Callbacks(), kSetupBudget, tables.get(), media.ident,
        media.identBytes, setup.data(), setup.size(), &result), hv_setup_destroy);
    EXPECT_EQ(rejected, nullptr); EXPECT_EQ(result, HV_UNSUPPORTED);
}

TEST(VorbisDecoder, RejectsZeroDimensionalMappedCodebooksBeforeLookupConstruction) {
    const auto fixture = harken::test::MakeVorbisFixture(1);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    Memory memory;
    Tables tables(hv_tables_create(memory.Callbacks(), kSetupBudget), hv_tables_destroy);
    ASSERT_NE(tables, nullptr);
    const auto tableAllocations = memory.live;
    for (unsigned map : {1u, 2u}) for (unsigned dimension : {1u, 0u}) {
        std::vector<std::uint8_t> setup{5, 'v', 'o', 'r', 'b', 'i', 's'};
        unsigned bit = 0;
        auto put = [&](std::uint32_t value, unsigned n) {
            for (unsigned i = 0; i < n; ++i, ++bit) {
                if ((bit & 7) == 0) setup.push_back(0);
                setup.back() |= ((value >> i) & 1) << (bit & 7);
            }
        };
        put(1,8);
        // The first mapped book is deliberately unused by floor and residue.
        put(0x564342,24); put(dimension,16); put(1,24);
        put(0,1); put(0,1); put(0,5); put(map,4);
        put(0,32); put(0,32); put(0,4); put(0,1);
        if (dimension) put(0,1);
        put(0x564342,24); put(1,16); put(1,24);
        put(0,1); put(0,1); put(0,5); put(0,4);
        put(0,6); put(0,16);
        put(0,6); put(1,16); put(0,5); put(0,2); put(6,4);
        put(0,6); put(1,16); put(0,24); put(0,24); put(0,24);
        put(0,6); put(1,8); put(0,3); put(0,1);
        put(0,6); put(0,16); put(0,1); put(0,1); put(0,2); put(0,8); put(0,8); put(0,8);
        put(0,6); put(0,1); put(0,16); put(0,16); put(0,8); put(1,1);
        HvResult result;
        DecoderSetup decoded(hv_setup_create(memory.Callbacks(), kSetupBudget, tables.get(), media.ident,
            media.identBytes, setup.data(), setup.size(), &result), hv_setup_destroy);
        EXPECT_EQ(result, dimension ? HV_OK : HV_INVALID) << map;
        EXPECT_EQ(decoded != nullptr, dimension != 0) << map;
        decoded.reset();
        EXPECT_EQ(memory.live, tableAllocations);
    }
}

TEST(VorbisDecoder, PoolBudgetReuseRetirementAndSharedSetupIdentity) {
    const auto fixture = harken::test::MakeVorbisFixture(2);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    harken::VorbisSettingsCache cache;
    ASSERT_TRUE(cache.Initialize(kSetupBudget));
    harken::VorbisPrepareResult result;
    auto a = cache.Prepare(media, kSetupBudget, result);
    ASSERT_NE(a, nullptr); ASSERT_EQ(result, harken::VorbisPrepareResult::Ok);
    auto b = cache.Prepare(media, kSetupBudget, result);
    ASSERT_EQ(a, b);
    const auto bytes = harken::VorbisStatePool::RequiredBytes(*a);
    harken::VorbisStatePool pool(bytes * 2);
    auto* first = pool.Acquire(a); auto* second = pool.Acquire(b);
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr); ASSERT_NE(first, second);
    EXPECT_EQ(pool.Acquire(a), nullptr); EXPECT_EQ(pool.ActiveBytes(), bytes * 2);
    first->Retire(); pool.Collect(); EXPECT_EQ(pool.ActiveBytes(), bytes);
    auto* reused = pool.Acquire(a); EXPECT_EQ(reused, first); EXPECT_EQ(pool.Bytes(), bytes * 2);
    std::weak_ptr<const harken::VorbisSettings> weak = a; a.reset(); b.reset();
    EXPECT_FALSE(weak.expired()); reused->Retire(); second->Retire(); pool.Collect();
    EXPECT_TRUE(weak.expired()); EXPECT_EQ(pool.ActiveBytes(), 0u);
    a = cache.Prepare(media, kSetupBudget, result); ASSERT_NE(a, nullptr);
    auto* cached = pool.Acquire(a);
    EXPECT_TRUE(cached == first || cached == second); EXPECT_EQ(pool.Bytes(), bytes * 2);
    cached->Retire(); pool.Collect();
}

TEST(VorbisDecoder, PoolReordersCanonicalChannelsWithoutAllocatingOnRenderRetirement) {
    harken::VorbisSettingsCache cache; ASSERT_TRUE(cache.Initialize(kSetupBudget));
    harken::VorbisWorkspace workspace; ASSERT_TRUE(workspace.Initialize());
    harken::VorbisStatePool pool(2 * 1024 * 1024);
    for (unsigned channels : {1u, 2u, 4u, 6u, 8u}) {
        const auto fixture = harken::test::MakeVorbisFixture(channels);
        harken::VorbisMedia media;
        ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
        harken::VorbisPrepareResult status;
        auto settings = cache.Prepare(media, kSetupBudget, status); ASSERT_NE(settings, nullptr);
        auto* state = pool.Acquire(settings); ASSERT_NE(state, nullptr);
        std::vector<float> actual(media.frames * channels);
        bool ok = true; std::uint64_t frame = 0;
        allocation_probe::count = 0; allocation_probe::enabled = true;
        {
            harken::memguard::AudioThreadScope audio;
            for (std::uint64_t at = 0; at < media.packetBytes;) {
                const std::uint8_t* packet; std::uint16_t bytes;
                if (!media.Packet(at, packet, bytes)) { ok = false; break; }
                const float* pcm[8]{}; unsigned count = 0;
                if (!state->Decode(workspace, packet, bytes, pcm, count)) { ok = false; break; }
                for (unsigned i = 0; i < count && frame < media.frames; ++i, ++frame)
                    for (unsigned ch = 0; ch < channels; ++ch) actual[frame * channels + ch] = pcm[ch][i];
                at += bytes + 2;
            }
            state->Retire();
        }
        allocation_probe::enabled = false;
        EXPECT_EQ(allocation_probe::count, 0u); ASSERT_TRUE(ok); ASSERT_EQ(frame, media.frames);
        static constexpr unsigned map6[] = {0, 2, 1, 5, 3, 4};
        static constexpr unsigned map8[] = {0, 2, 1, 7, 5, 6, 3, 4};
        for (std::uint64_t i = 0; i < media.frames; ++i) for (unsigned ch = 0; ch < channels; ++ch)
            ASSERT_EQ(actual[i * channels + ch], fixture.reference[i * channels +
                (channels == 6 ? map6[ch] : channels == 8 ? map8[ch] : ch)]);
        pool.Collect(); EXPECT_EQ(pool.ActiveBytes(), 0u);
    }
}

TEST(VorbisDecoder, DISABLED_DesktopDecodeTiming) {
    const auto fixture = harken::test::MakeVorbisFixture(2);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    harken::VorbisSettingsCache cache; ASSERT_TRUE(cache.Initialize(kSetupBudget));
    harken::VorbisWorkspace workspace; ASSERT_TRUE(workspace.Initialize());
    harken::VorbisPrepareResult status;
    auto settings = cache.Prepare(media, kSetupBudget, status); ASSERT_NE(settings, nullptr);
    for (unsigned voices : {1u, 128u}) {
        const auto stateBytes = harken::VorbisStatePool::RequiredBytes(*settings);
        harken::VorbisStatePool pool(stateBytes * voices);
        std::vector<harken::VorbisStatePool::State*> states;
        for (unsigned i = 0; i < voices; ++i) { states.push_back(pool.Acquire(settings)); ASSERT_NE(states.back(), nullptr); }
        std::vector<double> timings; timings.reserve(media.packetBytes / 2 * 10);
        bool ok = true; std::uint64_t produced = 0;
        for (unsigned repeat = 0; repeat < 10; ++repeat) {
            for (auto* state : states) state->Restart();
            for (std::uint64_t at = 0; at < media.packetBytes;) {
                const std::uint8_t* packet; std::uint16_t bytes; ASSERT_TRUE(media.Packet(at, packet, bytes));
                const auto begin = std::chrono::steady_clock::now();
                for (auto* state : states) {
                    const float* pcm[8]{}; unsigned count = 0;
                    if (!state->Decode(workspace, packet, bytes, pcm, count)) ok = false;
                    produced += count;
                }
                const auto end = std::chrono::steady_clock::now();
                timings.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
                at += bytes + 2;
            }
        }
        ASSERT_TRUE(ok); double total = 0; for (auto time : timings) total += time;
        std::sort(timings.begin(), timings.end());
        std::printf("decoder_timing voices=%u channels=2 rate=48000 packets=%zu mean_us=%.3f p99_us=%.3f max_us=%.3f ns_per_source_frame=%.3f state_pool_bytes=%zu setup_bytes=%zu tables_bytes=%zu workspace_bytes=%zu workspace_peak=%zu\n",
            voices, timings.size(), total / timings.size(), timings[timings.size() * 99 / 100], timings.back(),
            total * 1000 / produced, pool.Bytes(), settings->Bytes(), cache.TableBytes(), workspace.Bytes(), workspace.HighWater());
    }
}

TEST(VorbisDecoder, ExactAcrossLayoutsSeeksRestartsAndSharedSettings) {
    Memory memory;
    {
        Tables tables(hv_tables_create(memory.Callbacks(), kSetupBudget), hv_tables_destroy);
        ASSERT_NE(tables, nullptr);
        auto scratch = Storage(hv_workspace_bytes());
        auto* workspace = hv_workspace_init(scratch.data(), scratch.size() * sizeof(std::max_align_t));
        ASSERT_NE(workspace, nullptr);
        for (unsigned channels : {1u, 2u, 4u, 6u, 8u}) {
            SCOPED_TRACE(channels);
            auto fixture = harken::test::MakeVorbisFixture(channels, true, 48713, 95019);
            harken::VorbisMedia media;
            ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
            HvResult result = HV_INVALID;
            DecoderSetup setup(hv_setup_create(memory.Callbacks(), kSetupBudget, tables.get(), media.ident,
                media.identBytes, media.setup, media.setupBytes, &result), hv_setup_destroy);
            ASSERT_EQ(result, HV_OK); ASSERT_NE(setup, nullptr);
            auto backing = Storage(hv_state_bytes(setup.get()));
            auto* state = hv_state_init(backing.data(), backing.size() * sizeof(std::max_align_t), setup.get());
            ASSERT_NE(state, nullptr);
            for (auto target : {0ull, 48713ull, 4095ull, 95018ull, 143001ull, 1ull}) {
                SCOPED_TRACE(target);
                hv_state_restart(state);
                const auto* point = media.Seek(target);
                auto skip = point->discardFrames + target - point->frame;
                auto offset = point->packetOffset;
                std::uint64_t frame = target;
                while (frame < media.frames) {
                    const std::uint8_t* packet; std::uint16_t bytes;
                    ASSERT_TRUE(media.Packet(offset, packet, bytes));
                    float** pcm = nullptr; unsigned count = 0;
                    ASSERT_EQ(hv_decode_packet(state, workspace, packet, bytes, &pcm, &count), HV_OK);
                    const auto discard = std::min<std::uint64_t>(skip, count); skip -= discard;
                    for (unsigned i = static_cast<unsigned>(discard); i < count && frame < media.frames; ++i, ++frame)
                        for (unsigned ch = 0; ch < channels; ++ch)
                            ASSERT_EQ(pcm[ch][i], fixture.reference[frame * channels + ch]) << frame << '/' << ch;
                    offset += bytes + 2;
                }
            }
            std::printf("decoder_memory channels=%u tables=%zu setup=%zu setup_peak=%zu state=%zu workspace_capacity=%zu workspace_high_water=%zu\n",
                channels, hv_tables_bytes(tables.get()), hv_setup_bytes(setup.get()), hv_setup_peak_bytes(setup.get()),
                hv_state_bytes(setup.get()), hv_workspace_bytes(), hv_workspace_high_water(workspace));
        }
    }
    EXPECT_EQ(memory.live, 0u);
}

TEST(VorbisDecoder, RenderUsesNoAllocatorAndExistingAudioMemoryGuard) {
    Memory memory;
    Tables tables(hv_tables_create(memory.Callbacks(), kSetupBudget), hv_tables_destroy);
    ASSERT_NE(tables, nullptr);
    const auto fixture = harken::test::MakeVorbisFixture(8);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    HvResult result;
    DecoderSetup setup(hv_setup_create(memory.Callbacks(), kSetupBudget, tables.get(), media.ident,
        media.identBytes, media.setup, media.setupBytes, &result), hv_setup_destroy);
    ASSERT_EQ(result, HV_OK); ASSERT_NE(setup, nullptr);
    auto backing = Storage(hv_state_bytes(setup.get())); auto scratch = Storage(hv_workspace_bytes());
    auto* state = hv_state_init(backing.data(), backing.size() * sizeof(std::max_align_t), setup.get());
    auto* workspace = hv_workspace_init(scratch.data(), scratch.size() * sizeof(std::max_align_t));
    ASSERT_NE(state, nullptr); ASSERT_NE(workspace, nullptr);
    const auto before = memory.allocations;
    const bool wasArmed = harken::memguard::IsArmed(); harken::memguard::Install();
    harken::memguard::AllocationObservation observed{};
    bool ok = true; allocation_probe::count = 0; allocation_probe::enabled = true;
    {
        harken::memguard::AllocationObservationScope observation(observed);
        harken::memguard::AudioThreadScope audio;
        for (unsigned repeat = 0; repeat < 3 && ok; ++repeat) {
            hv_state_restart(state);
            for (std::uint64_t offset = 0; offset < media.packetBytes;) {
                const std::uint8_t* packet; std::uint16_t bytes;
                if (!media.Packet(offset, packet, bytes)) { ok = false; break; }
                float** pcm = nullptr; unsigned count = 0;
                if (hv_decode_packet(state, workspace, packet, bytes, &pcm, &count) != HV_OK) { ok = false; break; }
                offset += bytes + 2;
            }
        }
    }
    allocation_probe::enabled = false;
    if (!wasArmed) harken::memguard::Uninstall();
    EXPECT_TRUE(ok); EXPECT_EQ(memory.allocations, before); EXPECT_EQ(allocation_probe::count, 0u);
    EXPECT_EQ(observed.audio.allocations + observed.audio.reallocations, 0u);
#if defined(_DEBUG) && defined(_MSC_VER)
    EXPECT_TRUE(observed.available);
#endif
    std::printf("decoder_audio_guard available=%d allocator_calls=%zu cpp_allocations=%zu\n",
        observed.available, memory.allocations - before, allocation_probe::count);
}

TEST(VorbisDecoder, PreparationFailureReleasesEveryAllocationAndRejectsUnsupportedChannels) {
    const auto fixture = harken::test::MakeVorbisFixture(2);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    Memory memory;
    Tables tables(hv_tables_create(memory.Callbacks(), kSetupBudget), hv_tables_destroy);
    ASSERT_NE(tables, nullptr);
    const auto baselineLive = memory.live;
    HvResult result;
    {
        DecoderSetup setup(hv_setup_create(memory.Callbacks(), 1, tables.get(), media.ident,
            media.identBytes, media.setup, media.setupBytes, &result), hv_setup_destroy);
        EXPECT_EQ(setup, nullptr); EXPECT_EQ(result, HV_MEMORY);
    }
    const auto before = memory.allocations;
    {
        DecoderSetup valid(hv_setup_create(memory.Callbacks(), kSetupBudget, tables.get(), media.ident,
            media.identBytes, media.setup, media.setupBytes, &result), hv_setup_destroy);
        ASSERT_EQ(result, HV_OK); ASSERT_NE(valid, nullptr);
    }
    const auto preparationAllocations = memory.allocations - before;
    for (std::size_t fail = 1; fail <= preparationAllocations; ++fail) {
        memory.failAt = memory.allocations + fail;
        DecoderSetup setup(hv_setup_create(memory.Callbacks(), kSetupBudget, tables.get(), media.ident,
            media.identBytes, media.setup, media.setupBytes, &result), hv_setup_destroy);
        EXPECT_EQ(setup, nullptr); EXPECT_EQ(result, HV_MEMORY);
        EXPECT_EQ(memory.live, baselineLive) << fail;
    }
    memory.failAt = 0;
    std::vector<std::uint8_t> ident(media.ident, media.ident + media.identBytes);
    for (unsigned channel : {3u, 5u, 7u}) {
        ident[11] = static_cast<std::uint8_t>(channel);
        DecoderSetup setup(hv_setup_create(memory.Callbacks(), kSetupBudget, tables.get(), ident.data(), ident.size(),
            media.setup, media.setupBytes, &result), hv_setup_destroy);
        EXPECT_EQ(setup, nullptr); EXPECT_EQ(result, HV_UNSUPPORTED); EXPECT_EQ(memory.live, baselineLive);
    }
}

TEST(VorbisDecoder, ResidentCodecSeekAndRetirementUseNoAudioThreadAllocation) {
    const auto fixture = harken::test::MakeVorbisFixture(6, false, 257, 1023);
    harken::VorbisCodec codec(2 * 1024 * 1024, 0);
    harken::VorbisPrepareResult result;
    auto source = harken::VorbisResident::Prepare(codec, fixture.media, kSetupBudget, result);
    ASSERT_NE(source, nullptr);
    auto* decoder = source->Acquire(); ASSERT_NE(decoder, nullptr);
    const bool armed = harken::memguard::IsArmed(); harken::memguard::Install();
    harken::memguard::AllocationObservation observed{};
    bool ok = true;
    allocation_probe::count = 0; allocation_probe::enabled = true;
    {
        harken::memguard::AllocationObservationScope observation(observed);
        harken::memguard::AudioThreadScope audio;
        for (std::uint64_t start : {0u, 517u, 257u, 9001u, 0u}) {
            auto at = start;
            while (at < start + 1024) {
                harken::PCMView output;
                if (!decoder->Read(at, 64, 0, output) || !output.frames) { ok = false; break; }
                at += output.frames;
            }
        }
        decoder->Retire();
    }
    allocation_probe::enabled = false;
    if (!armed) harken::memguard::Uninstall();
    EXPECT_TRUE(ok); EXPECT_EQ(allocation_probe::count, 0u);
    EXPECT_EQ(observed.audio.allocations + observed.audio.reallocations, 0u);
#if defined(_DEBUG) && defined(_MSC_VER)
    EXPECT_TRUE(observed.available);
#endif
    codec.states.Collect(); EXPECT_EQ(codec.states.ActiveBytes(), 0u);
}

TEST(VorbisDecoder, RejectsSeekLoopAndDurationMetadataThatDisagreesWithPackets) {
    const auto fixture = harken::test::MakeVorbisFixture(2, true, 48713, 95019);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    harken::VorbisSettingsCache cache; ASSERT_TRUE(cache.Initialize(kSetupBudget));
    harken::VorbisPrepareResult result;
    auto valid = cache.Prepare(media, kSetupBudget, result); ASSERT_NE(valid, nullptr);
    EXPECT_TRUE(valid->ValidateResident(media));
    auto bad = media; ++bad.seek[1].discardFrames;
    EXPECT_FALSE(valid->ValidateResident(bad));
    bad = media; ++bad.loopTrim;
    EXPECT_FALSE(valid->ValidateResident(bad));
    bad = media; ++bad.loopDiscard;
    EXPECT_FALSE(valid->ValidateResident(bad));
    bad = media; bad.frames -= 4096;
    EXPECT_FALSE(valid->ValidateResident(bad));
    EXPECT_EQ(cache.Prepare(media, kSetupBudget, result), valid);
}

TEST(VorbisDecoder, ByteRingWrapSeekAndGenerationReplacementMatchReference) {
    harken::VorbisCodec codec(2 * 1024 * 1024, 0);
    for (const unsigned channels : {1u, 2u, 4u, 6u, 8u}) {
        SCOPED_TRACE(channels);
        const auto fixture = harken::test::MakeVorbisFixture(channels, true);
        harken::VorbisMedia media;
        ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
        harken::VorbisPrepareResult result;
        auto setup = codec.settings.Prepare(media, kSetupBudget, result);
        ASSERT_NE(setup, nullptr);
        auto* decoder = codec.AcquireStream(setup);
        ASSERT_NE(decoder, nullptr);
        std::vector<std::uint8_t> storage(8192);
        harken::StreamRing ring;
        ring.Wire(storage.data(), static_cast<std::uint32_t>(storage.size()));
        // Reusing the same physical ring must discard the previous generation's
        // partially consumed decoded packet as well as its overlap history.
        for (const std::uint64_t target : {0u, 713u, 4097u}) {
            SCOPED_TRACE(target);
            ring.ResetForUse(channels);
            ring.readIndex.store(storage.size() - 7);
            ring.writeIndex.store(storage.size() - 7);
            struct Record { harken::StreamPacketHeader header; const std::uint8_t* packet; };
            std::vector<Record> records;
            const auto* seek = media.Seek(target);
            ASSERT_NE(seek, nullptr);
            auto skip = target - seek->frame + seek->discardFrames;
            std::uint64_t supplied = 0;
            long previousBlock = 0;
            for (auto at = seek->packetOffset; at < media.packetBytes && supplied < 4096;) {
                const std::uint8_t* packet = nullptr; std::uint16_t bytes = 0;
                ASSERT_TRUE(media.Packet(at, packet, bytes));
                const auto block = setup->PacketBlock(packet, bytes);
                ASSERT_GT(block, 0);
                const auto decoded = previousBlock ? (previousBlock + block) / 4 : 0;
                const auto discard = std::min<std::uint64_t>(skip, decoded);
                skip -= discard;
                const auto frames = std::min<std::uint64_t>(decoded - discard, media.frames - target - supplied);
                records.push_back({{bytes, static_cast<std::uint32_t>(frames),
                    static_cast<std::uint32_t>(discard), previousBlock ? 0u : 1u}, packet});
                supplied += frames;
                previousBlock = block;
                at += 2u + bytes;
            }
            ASSERT_GE(supplied, 4096u);
            ASSERT_EQ(records.front().header.frames, 0u);
            std::vector<float> actual(1001 * channels);
            std::size_t next = 0;
            unsigned copied = 0;
            bool valid = true;
            const bool armed = harken::memguard::IsArmed(); harken::memguard::Install();
            harken::memguard::AllocationObservation observed{};
            allocation_probe::count = 0; allocation_probe::enabled = true;
            {
                harken::memguard::AllocationObservationScope observation(observed);
                harken::memguard::AudioThreadScope audio;
                harken::PCMView preroll;
                while (next < records.size() && !records[next].header.frames) {
                    const auto& zero = records[next++];
                    const auto at = ring.writeIndex.load();
                    ring.CopyIn(at, &zero.header, sizeof(zero.header));
                    ring.CopyIn(at + sizeof(zero.header), zero.packet, zero.header.packetBytes);
                    ring.PublishBytes(static_cast<std::uint32_t>(sizeof(zero.header)) + zero.header.packetBytes, 0);
                    if (decoder->ReadStream(ring, 37, 0, preroll) != harken::StreamDecodeResult::Dry ||
                        ring.ReadAvailableBytes() != 0) valid = false;
                }
                const auto& pending = records[next++];
                const auto pendingAt = ring.writeIndex.load();
                const auto pendingBytes = static_cast<std::uint32_t>(sizeof(pending.header)) + pending.header.packetBytes;
                ring.CopyIn(pendingAt, &pending.header, sizeof(pending.header));
                ring.CopyIn(pendingAt + sizeof(pending.header), pending.packet, pending.header.packetBytes);
                ring.PublishBytes(pendingBytes, 0);
                if (!pending.header.frames ||
                    decoder->ReadStream(ring, 37, 0, preroll) != harken::StreamDecodeResult::Dry ||
                    ring.readIndex.load() != pendingAt) valid = false;
                ring.CaptureConsumerSnapshot();
                ring.PublishBytes(0, pending.header.frames);
                if (decoder->ReadStream(ring, 37, 0, preroll) != harken::StreamDecodeResult::Dry ||
                    ring.readIndex.load() != pendingAt) valid = false;
                ring.ClearConsumerSnapshot();
                for (unsigned attempt = 0; copied < 1001 && attempt < 1000; ++attempt) {
                    while (next < records.size()) {
                        const auto& r = records[next];
                        const auto bytes = static_cast<std::uint32_t>(sizeof(r.header)) + r.header.packetBytes;
                        if (bytes > ring.WriteAvailableBytes()) break;
                        const auto at = ring.writeIndex.load();
                        ring.CopyIn(at, &r.header, sizeof(r.header));
                        ring.CopyIn(at + sizeof(r.header), r.packet, r.header.packetBytes);
                        ring.PublishBytes(bytes, r.header.frames);
                        ++next;
                    }
                    harken::PCMView output;
                    if (decoder->ReadStream(ring, std::min(37u, 1001 - copied), 0, output) !=
                            harken::StreamDecodeResult::Ready || !output.frames) { valid = false; break; }
                    for (unsigned frame = 0; frame < output.frames; ++frame)
                        for (unsigned ch = 0; ch < channels; ++ch)
                            actual[(copied + frame) * channels + ch] =
                                static_cast<const float*>(output.channels[ch])[frame];
                    copied += static_cast<unsigned>(output.frames);
                    ring.DeliverFrames(static_cast<std::uint32_t>(output.frames));
                }
            }
            allocation_probe::enabled = false;
            if (!armed) harken::memguard::Uninstall();
            EXPECT_EQ(allocation_probe::count, 0u);
            EXPECT_EQ(observed.audio.allocations + observed.audio.reallocations, 0u);
#if defined(_DEBUG) && defined(_MSC_VER)
            EXPECT_TRUE(observed.available);
#endif
            ASSERT_TRUE(valid); ASSERT_EQ(copied, 1001u);
            static constexpr unsigned map6[] = {0, 2, 1, 5, 3, 4};
            static constexpr unsigned map8[] = {0, 2, 1, 7, 5, 6, 3, 4};
            for (unsigned frame = 0; frame < copied; ++frame)
                for (unsigned ch = 0; ch < channels; ++ch)
                    ASSERT_EQ(actual[frame * channels + ch], fixture.reference[(target + frame) * channels +
                        (channels == 6 ? map6[ch] : channels == 8 ? map8[ch] : ch)]);
        }
        decoder->Retire(); codec.states.Collect();
        EXPECT_EQ(codec.states.ActiveBytes(), 0u);
    }
}

}
