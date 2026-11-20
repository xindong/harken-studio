// VnConfig stream-field vintage acceptance and validation: an older
// caller whose structSize predates the stream fields must init cleanly
// with engine defaults no matter what bytes sit past its struct end; a
// full-vintage caller's explicit values must land in the resolved
// config; zeros keep selecting defaults; and the sanity ceilings —
// per-field and the combined ring-plus-staging byte product over the
// capacities each stream slot can consume — must refuse before allocation.
#include "Common/TestUtil.h"

#include "harken/core/EngineConfig.h"

#include <cstddef>
#include <cstdint>

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;

// The stream fields were appended in one segment, so the previous vintage
// ends exactly where the first of them begins.
const std::uint32_t kPreStreamVintageSize =
    static_cast<std::uint32_t>(offsetof(VnConfig, streamRingFrames));

TEST(StreamConfig, OldVintageIgnoresStreamFieldGarbage) {
    VnConfig config = MakeNullConfig();
    config.structSize = kPreStreamVintageSize;
    // Deliberately poison every appended field with values a full-vintage
    // caller would be refused for: if the engine wrongly read past the
    // declared struct end, Init would fail instead of defaulting.
    config.streamRingFrames = 0xffffffffu;
    config.maxStreams = 0xffffffffu;
    config.prebufferMs = 0xffffffffu;
    config.starvationCooldownBlocks = 0xffffffffu;
    config.maxPacks = 0xffffffffu;

    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const harken::EngineConfig& resolved = session.Engine()->config;
    EXPECT_EQ(16384u, resolved.streamRingFrames);
    EXPECT_EQ(16u, resolved.maxStreams);
    EXPECT_EQ(100u, resolved.prebufferMs);
    EXPECT_EQ(32u, resolved.starvationCooldownBlocks);
    EXPECT_EQ(256u, resolved.maxPacks);
}

TEST(StreamConfig, FullVintageStreamFieldsResolve) {
    VnConfig config = MakeNullConfig();
    config.streamRingFrames = 2048;
    config.maxStreams = 8;
    config.prebufferMs = 50;
    config.starvationCooldownBlocks = 16;
    config.maxPacks = 8;

    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const harken::EngineConfig& resolved = session.Engine()->config;
    EXPECT_EQ(2048u, resolved.streamRingFrames);
    EXPECT_EQ(8u, resolved.maxStreams);
    EXPECT_EQ(50u, resolved.prebufferMs);
    EXPECT_EQ(16u, resolved.starvationCooldownBlocks);
    EXPECT_EQ(8u, resolved.maxPacks);
}

TEST(StreamConfig, ZeroStreamFieldsSelectDefaults) {
    // Full vintage, all stream fields zero: same defaults as the old
    // vintage — zero means "engine chooses" at every vintage.
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const harken::EngineConfig& resolved = session.Engine()->config;
    EXPECT_EQ(16384u, resolved.streamRingFrames);
    EXPECT_EQ(16u, resolved.maxStreams);
    EXPECT_EQ(100u, resolved.prebufferMs);
    EXPECT_EQ(32u, resolved.starvationCooldownBlocks);
    EXPECT_EQ(256u, resolved.maxPacks);
}

TEST(StreamConfig, PerFieldCeilingsRefused) {
    struct OverCeiling {
        const char* name;
        void (*mutate)(VnConfig*);
    };
    const OverCeiling cases[] = {
        {"streamRingFrames above 1<<22",
         [](VnConfig* c) { c->streamRingFrames = (1u << 22) + 1u; }},
        {"maxStreams above 1<<12",
         [](VnConfig* c) { c->maxStreams = (1u << 12) + 1u; }},
        {"prebufferMs above one minute",
         [](VnConfig* c) { c->prebufferMs = 60001u; }},
        {"starvationCooldownBlocks above 1<<20",
         [](VnConfig* c) {
             c->starvationCooldownBlocks = (1u << 20) + 1u;
         }},
        {"maxPacks above 1<<12",
         [](VnConfig* c) { c->maxPacks = (1u << 12) + 1u; }},
    };
    for (const OverCeiling& over : cases) {
        VnConfig config = MakeNullConfig();
        over.mutate(&config);
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config)) << over.name;
    }
}

TEST(StreamConfig, StreamMemoryCeilingUsesRoundedRingCapacity) {
    // Each factor here clears its own ceiling, so only the combined byte
    // ceiling can refuse the combination. 2^19+1 frames round up to 2^20
    // for the power-of-two ring. Across 9 streams the rounded rings plus
    // staging require 360 MiB, while raw rings plus staging require about
    // 216 MiB. A refusal therefore proves the budget sees ring capacity.
    VnConfig config = MakeNullConfig();
    config.streamRingFrames = (1u << 19) + 1u;
    config.maxStreams = 9;
    EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));

    // Controls: each half of the refused combination is acceptable on its
    // own, so the refusal above isolates the product, not either field.
    {
        VnConfig control = MakeNullConfig();
        control.streamRingFrames = (1u << 19) + 1u;
        control.maxStreams = 1;
        EngineSession session(control);
        EXPECT_EQ(VN_OK, session.InitResult());
    }
    {
        VnConfig control = MakeNullConfig();
        control.maxStreams = 9;
        EngineSession session(control);
        EXPECT_EQ(VN_OK, session.InitResult());
    }
}

TEST(StreamConfig, StreamMemoryCeilingIncludesPerSlotStaging) {
    // At default ring depth each slot reserves 0.5 MiB of ring storage and
    // can grow an 8 MiB whole-chunk staging buffer. The default 256 Virtual
    // Voices add 8 MiB of stream-park token storage. Twenty-nine stream slots
    // therefore fit while thirty cross the 256 MiB aggregate ceiling.
    // ResolveConfig isolates the budget arithmetic without allocating pools.
    VnConfig accepted = MakeNullConfig();
    accepted.maxStreams = 29;
    harken::EngineConfig resolved;
    EXPECT_EQ(VN_OK, harken::ResolveConfig(&accepted, &resolved));

    VnConfig refused = MakeNullConfig();
    refused.maxStreams = 30;
    EXPECT_EQ(VN_INVALID_CONFIG,
              harken::ResolveConfig(&refused, &resolved));
}

TEST(StreamConfig, StreamParkTokenPoolSharesStreamingMemoryCeiling) {
    VnConfig boundary = MakeNullConfig();
    boundary.maxStreams = 30;
    boundary.maxParkedVoices = 32;
    harken::EngineConfig resolved;
    EXPECT_EQ(VN_OK, harken::ResolveConfig(&boundary, &resolved));
    EXPECT_EQ(32u, resolved.voiceCapacity.Virtual());

    VnConfig overBudget = boundary;
    overBudget.maxParkedVoices = 33;
    harken::EngineConfig unchanged;
    unchanged.maxStreams = 7;
    EXPECT_EQ(VN_INVALID_CONFIG,
              harken::ResolveConfig(&overBudget, &unchanged));
    EXPECT_EQ(7u, unchanged.maxStreams)
        << "failed resolution must not partially publish config";
}

} // namespace
