#include "Common/TestUtil.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

using harkentest::MakeNullConfig;

constexpr std::array<std::uint32_t, 16> kConfigVintages = {
    32, 64, 80, 88, 112, 184, 192, 208, 224, 232, 240, 264, 280, 288, 344,
    352,
};
constexpr std::array<std::uint32_t, 18> kSnapshotVintages = {
    1832, 2560, 2568, 2656, 2680, 2752,
    3144, 3152, 3160, 3416, 3536, 4280,
    4288, 4296, 4320, 4336, 4376, 5400,
};

static_assert(offsetof(VnConfig, onNotification) == 32);
static_assert(offsetof(VnConfig, logCallback) == 64);
static_assert(offsetof(VnConfig, delayEffectPoolCapacity) == 80);
static_assert(offsetof(VnConfig, streamRingFrames) == 88);
static_assert(offsetof(VnConfig, maxActions) == 112);
static_assert(offsetof(VnConfig, maxTriggerDepth) == 184);
static_assert(offsetof(VnConfig, mixChannelLayout) == 192);
static_assert(offsetof(VnConfig, maxBindings) == 208);
static_assert(offsetof(VnConfig, gainEffectPoolCapacity) == 224);
static_assert(offsetof(VnConfig, limiterEffectPoolCapacity) == 232);
static_assert(offsetof(VnConfig, compressorEffectPoolCapacity) == 240);
static_assert(offsetof(VnConfig, maxBusTopologyOwners) == 264);
static_assert(offsetof(VnConfig, maxActionFirings) == 280);
static_assert(offsetof(VnConfig, reserved10) == 284);
static_assert(offsetof(VnConfig, maxCatalogEntries) == 288);
static_assert(offsetof(VnConfig, reserved11) == 292);
static_assert(offsetof(VnConfig, maxCatalogBytes) == 296);
static_assert(offsetof(VnConfig, maxResidentDefinitionBytes) == 304);
static_assert(offsetof(VnConfig, maxCompositeIndexBytes) == 312);
static_assert(offsetof(VnConfig, maxPackDecodeStagingBytes) == 320);
static_assert(offsetof(VnConfig, maxPackResourceBytes) == 328);
static_assert(offsetof(VnConfig, maxPackReplacementPeakBytes) == 336);
static_assert(offsetof(VnConfig, maxActionCooldownRows) == 344);
static_assert(offsetof(VnConfig, reserved12) == 348);
static_assert(sizeof(VnConfig) == 352);
static_assert(sizeof(VnPackLoadDesc) == 56);
static_assert(offsetof(VnPackLoadDesc, structSize) == 0);
static_assert(offsetof(VnPackLoadDesc, flags) == 4);
static_assert(offsetof(VnPackLoadDesc, packStableId) == 8);
static_assert(offsetof(VnPackLoadDesc, utf8Path) == 16);
static_assert(offsetof(VnPackLoadDesc, utf8ContainmentRoot) == 24);
static_assert(offsetof(VnPackLoadDesc, providerUserData) == 32);
static_assert(offsetof(VnPackLoadDesc, provide) == 40);
static_assert(offsetof(VnPackLoadDesc, release) == 48);
static_assert(sizeof(VnInitPackInspectReport) == 80);
static_assert(offsetof(VnInitPackInspectReport, structSize) == 0);
static_assert(offsetof(VnInitPackInspectReport, status) == 4);
static_assert(offsetof(VnInitPackInspectReport, result) == 8);
static_assert(offsetof(VnInitPackInspectReport, reserved0) == 12);
static_assert(offsetof(VnInitPackInspectReport, packStableId) == 16);
static_assert(offsetof(VnInitPackInspectReport, catalogEntries) == 24);
static_assert(offsetof(VnInitPackInspectReport, catalogBytes) == 32);
static_assert(offsetof(VnInitPackInspectReport, recommendedMaxCatalogEntries) == 40);
static_assert(offsetof(VnInitPackInspectReport, recommendedMaxActions) == 44);
static_assert(offsetof(VnInitPackInspectReport, recommendedCatalogBytes) == 48);
static_assert(offsetof(VnInitPackInspectReport, recommendedResidentDefinitionBytes) == 56);
static_assert(offsetof(VnInitPackInspectReport, requiredCapabilityMask) == 64);
static_assert(offsetof(VnInitPackInspectReport, semanticFingerprint) == 72);

static_assert(offsetof(VnSnapshot, voicePublicHandles) == 1832);
static_assert(offsetof(VnSnapshot, logicLogDropCount) == 2560);
static_assert(offsetof(VnSnapshot, nestedTriggerDroppedCount) == 2568);
static_assert(offsetof(VnSnapshot, mixChannelCount) == 2656);
static_assert(offsetof(VnSnapshot, bindingDefinitionHighWater) == 2680);
static_assert(offsetof(VnSnapshot, effectTypeStatCount) == 2752);
static_assert(offsetof(VnSnapshot, busMuteMask) == 3144);
static_assert(
    offsetof(VnSnapshot, actionEmitterMultichannelFallbackCount) == 3152);
static_assert(offsetof(VnSnapshot, busPeaksPreFader) == 3160);
static_assert(offsetof(VnSnapshot, dynamicRouteCurrent) == 3416);
static_assert(offsetof(VnSnapshot, activeBusCount) == 3536);
static_assert(offsetof(VnSnapshot, commandDeferredCount) == 4280);
static_assert(offsetof(VnSnapshot, logRingHighWater) == 4288);
static_assert(offsetof(VnSnapshot, logQuantumBudgetDropCount) == 4296);
static_assert(offsetof(VnSnapshot, deadlineMissQuanta) == 4320);
static_assert(offsetof(VnSnapshot, lateConsumedEvents) == 4328);
static_assert(offsetof(VnSnapshot, variableAcceptedCount) == 4336);
static_assert(offsetof(VnSnapshot, variableTerminationCount) == 4368);
static_assert(offsetof(VnSnapshot, busChannelPeaks) == 4376);
static_assert(sizeof(VnSnapshot) == 5400);

static_assert(sizeof(VnCurrentWorldLoadDesc) == 16);
static_assert(offsetof(VnCurrentWorldLoadDesc, structSize) == 0);
static_assert(offsetof(VnCurrentWorldLoadDesc, flags) == 4);
static_assert(offsetof(VnCurrentWorldLoadDesc, utf8Path) == 8);
static_assert(sizeof(VnCurrentWorldLoadReport) == 64);
static_assert(offsetof(VnCurrentWorldLoadReport, structSize) == 0);
static_assert(offsetof(VnCurrentWorldLoadReport, status) == 4);
static_assert(offsetof(VnCurrentWorldLoadReport, result) == 8);
static_assert(offsetof(VnCurrentWorldLoadReport, reserved0) == 12);
static_assert(offsetof(VnCurrentWorldLoadReport, priorGeneration) == 16);
static_assert(offsetof(VnCurrentWorldLoadReport, resultGeneration) == 24);
static_assert(
    offsetof(VnCurrentWorldLoadReport, runtimeSemanticFingerprint) == 32);
static_assert(offsetof(VnCurrentWorldLoadReport, capabilityMask) == 40);
static_assert(offsetof(VnCurrentWorldLoadReport, bodyBytes) == 48);
static_assert(offsetof(VnCurrentWorldLoadReport, bodyChecksum) == 56);

template <std::size_t N>
bool Contains(const std::array<std::uint32_t, N>& values,
              std::uint32_t candidate) {
    return std::find(values.begin(), values.end(), candidate) != values.end();
}

// The field that opens each vintage: its offset is the previous vintage's
// accepted size, so a caller declaring a size at or past the end of one of
// these must come back with it filled.
struct VintageBoundary {
    const char* name;
    std::size_t offset;
    std::size_t bytes;
};

#define HARKEN_SNAPSHOT_BOUNDARY(field)                                       \
    VintageBoundary {                                                        \
        #field, offsetof(VnSnapshot, field), sizeof(VnSnapshot::field)       \
    }

const std::array<VintageBoundary, 15> kSnapshotBoundaries = {{
    HARKEN_SNAPSHOT_BOUNDARY(voicePublicHandles),
    HARKEN_SNAPSHOT_BOUNDARY(logicLogDropCount),
    HARKEN_SNAPSHOT_BOUNDARY(nestedTriggerDroppedCount),
    HARKEN_SNAPSHOT_BOUNDARY(mixChannelCount),
    HARKEN_SNAPSHOT_BOUNDARY(bindingDefinitionHighWater),
    HARKEN_SNAPSHOT_BOUNDARY(effectTypeStatCount),
    HARKEN_SNAPSHOT_BOUNDARY(busMuteMask),
    HARKEN_SNAPSHOT_BOUNDARY(actionEmitterMultichannelFallbackCount),
    HARKEN_SNAPSHOT_BOUNDARY(busPeaksPreFader),
    HARKEN_SNAPSHOT_BOUNDARY(dynamicRouteCurrent),
    HARKEN_SNAPSHOT_BOUNDARY(activeBusCount),
    HARKEN_SNAPSHOT_BOUNDARY(commandDeferredCount),
    HARKEN_SNAPSHOT_BOUNDARY(deadlineMissQuanta),
    HARKEN_SNAPSHOT_BOUNDARY(variableAcceptedCount),
    HARKEN_SNAPSHOT_BOUNDARY(busChannelPeaks),
}};

#undef HARKEN_SNAPSHOT_BOUNDARY

bool AllPoison(const VnSnapshot& snapshot, std::size_t offset,
               std::size_t bytes, std::uint8_t poison) {
    const auto* first =
        reinterpret_cast<const std::uint8_t*>(&snapshot) + offset;
    return std::all_of(first, first + bytes,
                       [poison](std::uint8_t v) { return v == poison; });
}

TEST(AbiVintageContract,
     CurrentWorldLoadValidatesArgumentsBeforeEngineAndZerosReport) {
    VnCurrentWorldLoadReport report{};
    report.structSize = sizeof(report);
    std::memset(reinterpret_cast<std::uint8_t*>(&report) + sizeof(uint32_t),
                0xA5, sizeof(report) - sizeof(uint32_t));

    EXPECT_EQ(VN_INVALID_ARG, Vn_LoadCurrentWorld(nullptr, &report));
    EXPECT_EQ(sizeof(report), report.structSize);
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_INVALID_ARGUMENT, report.status);
    EXPECT_EQ(VN_INVALID_ARG, report.result);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&report);
    EXPECT_TRUE(std::all_of(bytes + 12, bytes + sizeof(report),
                            [](std::uint8_t value) { return value == 0; }));

    VnCurrentWorldLoadDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Path = "does-not-exist.vnworld";
    report.structSize = sizeof(report);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_LoadCurrentWorld(&desc, &report));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_NOT_INITIALIZED, report.status);
    EXPECT_EQ(VN_NOT_INITIALIZED, report.result);

    desc.flags = 1;
    report.structSize = sizeof(report);
    EXPECT_EQ(VN_INVALID_ARG, Vn_LoadCurrentWorld(&desc, &report));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_INVALID_ARGUMENT, report.status);
    EXPECT_EQ(VN_INVALID_ARG, report.result);

    std::array<std::uint8_t, sizeof(report)> poisoned{};
    poisoned.fill(0xA5);
    auto* wrongSize =
        reinterpret_cast<VnCurrentWorldLoadReport*>(poisoned.data());
    wrongSize->structSize = sizeof(report) - 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_LoadCurrentWorld(&desc, wrongSize));
    EXPECT_TRUE(std::all_of(poisoned.begin() + sizeof(uint32_t),
                            poisoned.end(),
                            [](std::uint8_t value) { return value == 0xA5; }));
}

TEST(AbiVintageContract, ConfigAcceptsOnlyDeclaredDiscreteSizes) {
    constexpr std::uint8_t kPoison = 0xA5;
    for (const std::uint32_t size : kConfigVintages) {
        VnConfig config = MakeNullConfig(2, 256, 48000);
        if (size < sizeof(config)) {
            std::memset(
                reinterpret_cast<std::uint8_t*>(&config) + size,
                kPoison, sizeof(config) - size);
        }
        config.structSize = size;
        ASSERT_EQ(VN_OK, Vn_Init(&config)) << "accepted size " << size;
        EXPECT_EQ(VN_OK, Vn_Term()) << "accepted size " << size;
    }

    for (std::uint32_t size = 0;
         size <= sizeof(VnConfig) + 8; ++size) {
        if (Contains(kConfigVintages, size)) {
            continue;
        }
        VnConfig config = MakeNullConfig(2, 256, 48000);
        config.structSize = size;
        EXPECT_EQ(VN_INVALID_ARG, Vn_Init(&config))
            << "undeclared size " << size;
    }
    VnConfig oversized = MakeNullConfig(2, 256, 48000);
    oversized.structSize = (std::numeric_limits<std::uint32_t>::max)();
    EXPECT_EQ(VN_INVALID_ARG, Vn_Init(&oversized));
}

TEST(AbiVintageContract, SnapshotAcceptsOnlyDeclaredDiscreteSizesAndPreservesTail) {
    harkentest::EngineSession session(MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr std::uint8_t kPoison = 0xA5;
    for (const std::uint32_t size : kSnapshotVintages) {
        VnSnapshot snapshot;
        std::memset(&snapshot, kPoison, sizeof(snapshot));
        snapshot.structSize = size;
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot))
            << "accepted size " << size;

        const auto* bytes =
            reinterpret_cast<const std::uint8_t*>(&snapshot);
        EXPECT_TRUE(std::all_of(
            bytes + size, bytes + sizeof(snapshot),
            [](std::uint8_t value) { return value == kPoison; }))
            << "tail write beyond declared size " << size;

        // The other half of the sentinel. Asserting only that nothing past the
        // declared size was written is fail-open: a gate that withdraws a
        // field group from a caller that is entitled to it leaves poison in
        // the middle of the struct, with a clean tail, and passes. So every
        // vintage boundary field that fits inside the declared size has to
        // come back written.
        for (const VintageBoundary& boundary : kSnapshotBoundaries) {
            if (boundary.offset + boundary.bytes > size) {
                continue;
            }
            EXPECT_FALSE(AllPoison(snapshot, boundary.offset, boundary.bytes,
                                   kPoison))
                << "declared size " << size << " left " << boundary.name
                << " unwritten";
        }

        // Boundary fields alone are a thin sample: a group's opening field can
        // be written while a field behind it is skipped, and the boundary check
        // passes. So sweep the whole declared prefix for any field-width run of
        // poison instead of naming fields -- naming them would mean a future
        // append is only covered if someone remembers to extend the list, which
        // is the shape this ladder exists to avoid. Forgetting to classify a
        // new sparse array fails this sweep rather than silently widening the
        // blind spot.
        //
        // No region needs excluding today: the count-driven arrays (voice rows,
        // bus peaks) are written in full regardless of their companion count, so
        // an idle capture leaves no gap. A future array filled only to its count
        // would fail here and would have to be named and justified -- failing
        // closed, unlike a field list that silently stops covering an append.
        for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= size;
             offset += sizeof(std::uint32_t)) {
            EXPECT_FALSE(
                AllPoison(snapshot, offset, sizeof(std::uint32_t), kPoison))
                << "declared size " << size << " left offset " << offset
                << " unwritten (a field in a vintage the caller declared)";
        }
    }

    for (std::uint32_t size = 0;
         size <= sizeof(VnSnapshot) + 8; ++size) {
        if (Contains(kSnapshotVintages, size)) {
            continue;
        }
        VnSnapshot snapshot;
        std::memset(&snapshot, kPoison, sizeof(snapshot));
        snapshot.structSize = size;
        const VnSnapshot before = snapshot;
        EXPECT_EQ(VN_INVALID_ARG, Vn_CaptureSnapshot(&snapshot))
            << "undeclared size " << size;
        EXPECT_EQ(0, std::memcmp(&snapshot, &before, sizeof(snapshot)))
            << "rejected input was modified for size " << size;
    }

    VnSnapshot oversized;
    std::memset(&oversized, kPoison, sizeof(oversized));
    oversized.structSize =
        (std::numeric_limits<std::uint32_t>::max)();
    const VnSnapshot before = oversized;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CaptureSnapshot(&oversized));
    EXPECT_EQ(0, std::memcmp(&oversized, &before, sizeof(oversized)));
}

} // namespace
