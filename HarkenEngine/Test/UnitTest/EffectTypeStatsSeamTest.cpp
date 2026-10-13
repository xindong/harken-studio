// Frozen-contract pin for the per-type effect observability table, now
// published in harken/Harken.h as a bounded per-type stats table appended after
// VnSnapshot's tail counter:
//
//   uint32_t effectTypeStatCount;   // registered built-in type count
//   uint32_t reserved4;             // keeps 8-byte alignment
//   VnEffectTypeStats effectTypeStats[VN_MAX_EFFECT_TYPE_STATS]; // == 16 rows
//
//   VnEffectTypeStats {             // 24 bytes, 4-byte aligned
//     uint32_t effectType; uint32_t capacity; uint32_t activeCount;
//     uint32_t highWater;  uint32_t refusalCount; uint32_t reserved0;
//   }
//
// Rows cover every registered built-in type in effect-type order. The existing
// delayEffectHighWater / toneEffectHighWater scalars stay and must equal their
// rows' high-water. Refusal counters are cumulative per type and advance on
// every logic-side pool-exhaustion refusal, in isolation (a refusal of one type
// never bumps another type's counter).
//
// The harken_runtime_tests target defines HARKEN_EFFECT_TYPE_STATS_PUBLISHED (see
// CMakeLists.txt), so the compile-bound material below is live: the
// static_asserts pin the real header's frozen shape/size/offsets and each facet
// is a real runtime test. This file stays the authoritative frozen-contract pin
// — if the header ever drifts from the shape above, these asserts fail. The
// #else branch keeps this suite's red-test convention (UnitTest/RedTestGate.h,
// as in EffectContractSeamTest.cpp) intact for a build that leaves the macro
// undefined: visible skipped tests that turn red under HARKEN_RUN_RED_TESTS=1.
#include "Common/ObjectIdTestSupport.h"
#include "UnitTest/RedTestGate.h"

#include <harken/Harken.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

// Append-only protection for the legacy per-type scalars, always compiled: their
// byte offsets are frozen at today's layout so a publisher appending the new
// table cannot silently relocate an existing observability field. The literals
// were read from the compiler (offsetof over the current header), not guessed.
static_assert(offsetof(VnSnapshot, delayEffectHighWater) == 1776,
              "delayEffectHighWater offset is frozen; appending the per-type "
              "stats table must not move existing snapshot fields");
static_assert(offsetof(VnSnapshot, toneEffectHighWater) == 1784,
              "toneEffectHighWater offset is frozen; appending the per-type "
              "stats table must not move existing snapshot fields");

#ifdef HARKEN_EFFECT_TYPE_STATS_PUBLISHED
#include "Common/TestUtil.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <cstring>
#include <type_traits>
#include <vector>

// Row struct identity, size, and field layout, frozen.
static_assert(std::is_same_v<decltype(VnEffectTypeStats::effectType), std::uint32_t>,
              "VnEffectTypeStats::effectType must be uint32_t");
static_assert(std::is_same_v<decltype(VnEffectTypeStats::capacity), std::uint32_t>,
              "VnEffectTypeStats::capacity must be uint32_t");
static_assert(std::is_same_v<decltype(VnEffectTypeStats::activeCount), std::uint32_t>,
              "VnEffectTypeStats::activeCount must be uint32_t");
static_assert(std::is_same_v<decltype(VnEffectTypeStats::highWater), std::uint32_t>,
              "VnEffectTypeStats::highWater must be uint32_t");
static_assert(std::is_same_v<decltype(VnEffectTypeStats::refusalCount), std::uint32_t>,
              "VnEffectTypeStats::refusalCount must be uint32_t");
static_assert(std::is_same_v<decltype(VnEffectTypeStats::reserved0), std::uint32_t>,
              "VnEffectTypeStats::reserved0 must be uint32_t");
static_assert(sizeof(VnEffectTypeStats) == 24,
              "VnEffectTypeStats is six uint32_t fields, 24 bytes");
static_assert(offsetof(VnEffectTypeStats, effectType) == 0 &&
                  offsetof(VnEffectTypeStats, capacity) == 4 &&
                  offsetof(VnEffectTypeStats, activeCount) == 8 &&
                  offsetof(VnEffectTypeStats, highWater) == 12 &&
                  offsetof(VnEffectTypeStats, refusalCount) == 16 &&
                  offsetof(VnEffectTypeStats, reserved0) == 20,
              "VnEffectTypeStats field layout frozen: type, capacity, active, "
              "highWater, refusals, reserved");

// Table identity, capacity, and placement, frozen.
static_assert(VN_MAX_EFFECT_TYPE_STATS == 16,
              "the per-type stats table holds a fixed 16 rows");
static_assert(std::is_same_v<decltype(VnSnapshot::effectTypeStatCount), std::uint32_t>,
              "VnSnapshot::effectTypeStatCount must be uint32_t");
static_assert(std::is_same_v<decltype(VnSnapshot::reserved4), std::uint32_t>,
              "VnSnapshot::reserved4 must be uint32_t");
static_assert(std::extent_v<decltype(VnSnapshot::effectTypeStats)> ==
                  VN_MAX_EFFECT_TYPE_STATS,
              "effectTypeStats is a fixed 16-row array (bounded, append-only)");
static_assert(std::is_same_v<std::remove_extent_t<decltype(VnSnapshot::effectTypeStats)>,
                             VnEffectTypeStats>,
              "effectTypeStats rows are VnEffectTypeStats");
static_assert(offsetof(VnSnapshot, effectTypeStatCount) >
                  offsetof(VnSnapshot, meterLoopCount),
              "the per-type stats table appends after the current tail counter");
#endif

namespace {

#ifdef HARKEN_EFFECT_TYPE_STATS_PUBLISHED
using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

VnConfig StatsConfig(std::uint32_t delayCap, std::uint32_t toneCap) {
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.delayEffectPoolCapacity = delayCap;
    config.toneEffectPoolCapacity = toneCap;
    return config;
}

VnFeedbackDelayDesc DelayDesc() {
    VnFeedbackDelayDesc d{};
    d.structSize = sizeof(VnFeedbackDelayDesc);
    d.delayMs = 10.0f;
    d.feedback = 0.5f;
    d.wet = 1.0f;
    d.dry = 1.0f;
    return d;
}

VnThreeBandToneDesc ToneDesc() {
    VnThreeBandToneDesc t{};
    t.structSize = sizeof(VnThreeBandToneDesc);
    t.lowShelfGainDb = 6.0f;
    t.lowShelfFreqHz = 200.0f;
    t.peakGainDb = 0.0f;
    t.peakFreqHz = 1000.0f;
    t.peakQ = 1.0f;
    t.highShelfGainDb = 0.0f;
    t.highShelfFreqHz = 6000.0f;
    return t;
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

// Returns the table row for `type`, or nullptr if the table has no such row.
const VnEffectTypeStats* FindRow(const VnSnapshot& snap, VnEffectType type) {
    for (std::uint32_t i = 0; i < snap.effectTypeStatCount; ++i) {
        if (snap.effectTypeStats[i].effectType == type) {
            return &snap.effectTypeStats[i];
        }
    }
    return nullptr;
}

VnSnapshot TakeSnapshot() {
    VnSnapshot snap{};
    snap.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
    return snap;
}
#endif

// Facet (a): the table is present and complete for every released type, its row
// capacities match the configured pool capacities, active/high-water track real
// instance lifetimes (a hollow always-zero producer must fail), and the legacy
// scalars equal their rows' high-water at every observation point.
TEST(EffectTypeStatsSeam, PerTypeStatsTableTracksLiveInstances) {
#ifdef HARKEN_EFFECT_TYPE_STATS_PUBLISHED
    EngineSession session(StatsConfig(/*delayCap=*/4, /*toneCap=*/4));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(4000, 1000.0, kRate, 0.5f);
    const VnPCMDesc pcm = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    const VnFeedbackDelayDesc d = DelayDesc();
    const VnThreeBandToneDesc t = ToneDesc();

    VnVoiceHandle clearable = 0;
    for (int i = 0; i < 3; ++i) { // three feedback-delay instances
        VnVoiceHandle h = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
        ASSERT_EQ(VN_OK,
                  Vn_SetVoiceEffect(h, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d)));
        clearable = h;
    }
    for (int i = 0; i < 2; ++i) { // two three-band-tone instances
        VnVoiceHandle h = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
        ASSERT_EQ(VN_OK,
                  Vn_SetVoiceEffect(h, 0, VN_EFFECT_THREE_BAND_TONE, AsBase(&t)));
    }
    Capture warm;
    harkentest::PumpAndCapture(session.Engine(), 1, warm);

    VnSnapshot before = TakeSnapshot();
    // The published count is exactly the registered effect-type count, so the
    // [0, effectTypeStatCount) validity window covers every real row (a producer
    // publishing the full 16 with zero-filled tails would fail here).
    EXPECT_EQ(harken::kEffectRegistryRowCount, before.effectTypeStatCount)
        << "effectTypeStatCount must equal the registered effect-type count";
    const VnEffectTypeStats* delayRow = FindRow(before, VN_EFFECT_FEEDBACK_DELAY);
    const VnEffectTypeStats* toneRow = FindRow(before, VN_EFFECT_THREE_BAND_TONE);
    ASSERT_NE(nullptr, delayRow) << "delay row missing from per-type stats table";
    ASSERT_NE(nullptr, toneRow) << "tone row missing from per-type stats table";
    EXPECT_EQ(4u, delayRow->capacity) << "delay row capacity == configured cap";
    EXPECT_EQ(4u, toneRow->capacity) << "tone row capacity == configured cap";
    EXPECT_EQ(3u, delayRow->activeCount);
    EXPECT_EQ(3u, delayRow->highWater);
    EXPECT_EQ(2u, toneRow->activeCount);
    EXPECT_EQ(2u, toneRow->highWater);
    EXPECT_EQ(before.delayEffectHighWater, delayRow->highWater)
        << "legacy delay scalar must equal its row high-water";
    EXPECT_EQ(before.toneEffectHighWater, toneRow->highWater)
        << "legacy tone scalar must equal its row high-water";

    // Clearing one delay retires its instance: active falls, high-water holds.
    ASSERT_EQ(VN_OK, Vn_ClearVoiceEffect(clearable, 0));
    Capture drain;
    harkentest::PumpAndCapture(session.Engine(), 1, drain); // audio retires
    ASSERT_EQ(VN_OK, Vn_Update());                         // logic recycles

    VnSnapshot after = TakeSnapshot();
    delayRow = FindRow(after, VN_EFFECT_FEEDBACK_DELAY);
    toneRow = FindRow(after, VN_EFFECT_THREE_BAND_TONE);
    ASSERT_NE(nullptr, delayRow);
    ASSERT_NE(nullptr, toneRow);
    EXPECT_EQ(2u, delayRow->activeCount) << "one delay instance was cleared";
    EXPECT_EQ(3u, delayRow->highWater) << "high-water never recedes";
    EXPECT_EQ(2u, toneRow->activeCount) << "tone instances untouched";
    EXPECT_EQ(after.delayEffectHighWater, delayRow->highWater);
    EXPECT_EQ(after.toneEffectHighWater, toneRow->highWater);
#else
    constexpr const char* kPin =
        "Pins the per-type effect stats table: VnSnapshot exposes a bounded row "
        "count and rows carrying {type, capacity, active, highWater, refusals} "
        "for every released built-in type; the delay/tone row capacities equal "
        "delayEffectPoolCapacity/toneEffectPoolCapacity, activeCount/highWater "
        "track real instance lifetimes (clearing an instance drops active while "
        "high-water holds), and delayEffectHighWater/toneEffectHighWater equal "
        "their rows' high-water. Unskipped when the generated effect registry "
        "publishes the per-type stats table in harken/Harken.h and the build "
        "defines HARKEN_EFFECT_TYPE_STATS_PUBLISHED.";
    if (harkentest::RunRedTestsEnabled()) {
        FAIL() << "RED: " << kPin;
    }
    GTEST_SKIP() << kPin;
#endif
}

// Facet (b): a pool-exhaustion refusal increments the refusing type's refusal
// count cumulatively (not a boolean), and only that type's counter (per-type
// isolation). Exhausts a one-instance delay pool through the public API twice
// while leaving the tone pool untouched.
TEST(EffectTypeStatsSeam, PoolExhaustionIncrementsRefusalCountPerType) {
#ifdef HARKEN_EFFECT_TYPE_STATS_PUBLISHED
    EngineSession session(StatsConfig(/*delayCap=*/1, /*toneCap=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(4000, 1000.0, kRate, 0.5f);
    const VnPCMDesc pcm = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle a = 0, b = 0, c = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &b));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &c));

    const VnFeedbackDelayDesc d = DelayDesc();
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(a, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d)));

    // Before any refusal: both counters clean.
    VnSnapshot s0 = TakeSnapshot();
    const VnEffectTypeStats* delay0 = FindRow(s0, VN_EFFECT_FEEDBACK_DELAY);
    const VnEffectTypeStats* tone0 = FindRow(s0, VN_EFFECT_THREE_BAND_TONE);
    ASSERT_NE(nullptr, delay0);
    ASSERT_NE(nullptr, tone0);
    EXPECT_EQ(0u, delay0->refusalCount);
    EXPECT_EQ(0u, tone0->refusalCount);

    // Refusal #1: the one-instance delay pool is full.
    ASSERT_EQ(VN_NO_EFFECT,
              Vn_SetVoiceEffect(b, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d)));
    VnSnapshot s1 = TakeSnapshot();
    EXPECT_EQ(1u, FindRow(s1, VN_EFFECT_FEEDBACK_DELAY)->refusalCount);
    EXPECT_EQ(0u, FindRow(s1, VN_EFFECT_THREE_BAND_TONE)->refusalCount);

    // Refusal #2: the counter accumulates rather than latching at one.
    ASSERT_EQ(VN_NO_EFFECT,
              Vn_SetVoiceEffect(c, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d)));
    VnSnapshot s2 = TakeSnapshot();
    EXPECT_EQ(2u, FindRow(s2, VN_EFFECT_FEEDBACK_DELAY)->refusalCount)
        << "refusal counter is cumulative, not boolean";
    EXPECT_EQ(0u, FindRow(s2, VN_EFFECT_THREE_BAND_TONE)->refusalCount)
        << "a delay refusal must not touch the tone counter (per-type isolation)";
#else
    constexpr const char* kPin =
        "Pins refusal-count behavior (new behavior): every pool-exhaustion "
        "refusal increments the refusing type's refusal counter cumulatively "
        "(not a boolean), surfaced in that type's stats row, and never touches "
        "another type's counter. Exhausting a one-instance delay pool twice via "
        "the public API must show delay refusals 0 -> 1 -> 2 while the tone row "
        "stays 0. Unskipped when the generated effect registry publishes the "
        "per-type stats table with refusal counts in harken/Harken.h and the "
        "build defines HARKEN_EFFECT_TYPE_STATS_PUBLISHED.";
    if (harkentest::RunRedTestsEnabled()) {
        FAIL() << "RED: " << kPin;
    }
    GTEST_SKIP() << kPin;
#endif
}

// Facet (c): the generated registry is complete and self-consistent — every
// row is present in effect-type order, each released type's public parameter
// count agrees with the dispatch, each row reads its own EngineConfig capacity
// source, and every row is labelled. Detecting a PUBLIC effect id added to
// Harken.h with no manifest row is NOT done here (the compiled registry cannot
// see an id that has no row); that absence is owned by the
// effect_registry_manifest_check ctest, which parses Harken.h against the source
// table.
TEST(EffectTypeStatsSeam, GeneratedRegistryCompletenessProof) {
#ifdef HARKEN_EFFECT_TYPE_STATS_PUBLISHED
    ASSERT_GE(harken::kEffectRegistryRowCount, 4u)
        << "the four released built-in effects must have registry rows";

    // Distinct capacities so a row echoing the wrong source (or a shared value)
    // is caught, not masked by equal numbers.
    harken::EngineConfig config{};
    config.delayEffectPoolCapacity = 5;
    config.toneEffectPoolCapacity = 9;
    config.gainEffectPoolCapacity = 13;
    config.limiterEffectPoolCapacity = 17;

    bool sawDelay = false;
    bool sawTone = false;
    bool sawGain = false;
    bool sawLimiter = false;
    for (std::uint32_t i = 0; i < harken::kEffectRegistryRowCount; ++i) {
        const harken::EffectRegistryRow& row = harken::kEffectRegistry[i];
        EXPECT_EQ(i, row.effectType)
            << "registry rows must be present in ascending effect-type order";
        ASSERT_NE(nullptr, row.label);
        EXPECT_GT(std::strlen(row.label), 0u) << "row label must be non-empty";
        ASSERT_NE(nullptr, row.capacitySource);
        EXPECT_GT(std::strlen(row.capacitySource), 0u)
            << "row must name its capacity source";
        // The registry's parameter count and the public dispatch must agree.
        EXPECT_EQ(row.paramCount, harken::EffectParamCount(row.effectType));
        switch (row.effectType) {
        case VN_EFFECT_FEEDBACK_DELAY:
            sawDelay = true;
            EXPECT_EQ(harken::kDelayParamCount, row.paramCount);
            EXPECT_EQ(5u, row.capacity(config))
                << "delay row must read delayEffectPoolCapacity";
            break;
        case VN_EFFECT_THREE_BAND_TONE:
            sawTone = true;
            EXPECT_EQ(harken::kToneParamCount, row.paramCount);
            EXPECT_EQ(9u, row.capacity(config))
                << "tone row must read toneEffectPoolCapacity";
            break;
        case VN_EFFECT_GAIN:
            sawGain = true;
            EXPECT_EQ(harken::kGainParamCount, row.paramCount);
            EXPECT_EQ(13u, row.capacity(config))
                << "gain row must read gainEffectPoolCapacity";
            break;
        case VN_EFFECT_LIMITER:
            sawLimiter = true;
            EXPECT_EQ(harken::kLimiterParamCount, row.paramCount);
            EXPECT_EQ(17u, row.capacity(config))
                << "limiter row must read limiterEffectPoolCapacity";
            break;
        default:
            break; // future types: identity/order/label/paramCount checked above
        }
    }
    EXPECT_TRUE(sawDelay) << "feedback-delay row missing from the registry";
    EXPECT_TRUE(sawTone) << "three-band-tone row missing from the registry";
    EXPECT_TRUE(sawGain) << "gain row missing from the registry";
    EXPECT_TRUE(sawLimiter) << "limiter row missing from the registry";
#else
    constexpr const char* kPin =
        "Pins the generated registry completeness proof: every registry row "
        "present in effect-type order, public parameter counts agree, each row "
        "reads its own EngineConfig capacity source, labels non-empty. "
        "Unskipped when the build defines HARKEN_EFFECT_TYPE_STATS_PUBLISHED.";
    if (harkentest::RunRedTestsEnabled()) {
        FAIL() << "RED: " << kPin;
    }
    GTEST_SKIP() << kPin;
#endif
}

} // namespace
