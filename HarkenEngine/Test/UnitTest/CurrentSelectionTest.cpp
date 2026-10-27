#include "Common/ObjectIdTestSupport.h"
#include "harken/core/CurrentSelection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace {

std::string Hex(const std::uint8_t* bytes, std::size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 0x0f]);
    }
    return result;
}

harken::SelectionSeedInputV1 GoldenInput() {
    static const harken::SelectionPathNodeV1 path[] = {{
        harken::PlayableKindV1::Container,
        harkentest::ObjectId(UINT64_C(0x4142434445464748)),
        harkentest::ObjectId(UINT64_C(0x5152535455565758)),
        harkentest::ObjectId(UINT64_C(0x6162636465666768)),
        UINT64_C(0x7172737475767778),
        UINT64_C(0x8182838485868788),
        6,
    }};
    harken::SelectionSeedInputV1 input{};
    input.worldSemanticFingerprint = UINT64_C(0x0102030405060708);
    input.programStableId = harkentest::ObjectId(UINT64_C(0x1112131415161718));
    input.programSemanticFingerprint = UINT64_C(0x2122232425262728);
    input.memoryScope = harken::MemoryScopeV1::Emitter;
    input.emitterSlot = 7;
    input.emitterGeneration = 9;
    input.rootStableId = harkentest::ObjectId(UINT64_C(0x3132333435363738));
    input.acceptedPostOrdinal = 5;
    input.pathCount = 1;
    input.path = path;
    input.occurrenceOrdinal = 3;
    input.loopIteration = 4;
    input.discipline = harken::SelectionDisciplineV1::Rotation;
    return input;
}

struct MemoryProgram {
    std::array<harken::ContainerChildProgramViewV2, 3> children{};
    harken::ContainerProgramViewV3 program{};

    explicit MemoryProgram(
        harken::ContainerModeV1 mode = harken::ContainerModeV1::Random,
        harken::RandomDisciplineV1 discipline =
            harken::RandomDisciplineV1::Draw) {
        children[0].stableChildOrdinal = 2;
        children[0].authoredStableOrder = 20;
        children[1].stableChildOrdinal = 5;
        children[1].authoredStableOrder = 5;
        children[2].stableChildOrdinal = 9;
        children[2].authoredStableOrder = 10;
        program.containerStableId = harkentest::ObjectId(100);
        program.programSemanticFingerprint = 200;
        program.mode = mode;
        program.randomDiscipline = discipline;
        program.memoryScope = harken::MemoryScopeV1::Emitter;
        program.childCount = static_cast<std::uint32_t>(children.size());
        program.children = children.data();
        if (mode == harken::ContainerModeV1::Random &&
            discipline == harken::RandomDisciplineV1::Draw) {
            children[0].frequencyWeight = 2;
            children[1].frequencyWeight = 1;
            children[2].frequencyWeight = 3;
            program.avoidLastN = 1;
        } else if (mode == harken::ContainerModeV1::Random) {
            children[0].orderWeight = 1;
            children[1].orderWeight = 2;
            children[2].orderWeight = 3;
            program.avoidLastN = 1;
        }
    }
};

harken::SelectionMemoryConfigV1 MemoryConfig() {
    harken::SelectionMemoryConfigV1 config{};
    config.maxEmitters = 2;
    config.maxGlobalRows = 2;
    config.maxEmitterRowsPerEmitter = 2;
    config.maxPendingDeltasTotal = 8;
    config.maxPendingGlobalPerRow = 4;
    config.maxPendingEmitterPerRow = 4;
    config.maxChildren = 3;
    config.maxAvoidLastN = 2;
    config.maxPathNodes = 2;
    return config;
}

harken::SelectionMemoryKeyV1 MemoryKey(
    harken::MemoryScopeV1 scope = harken::MemoryScopeV1::Emitter,
    std::uint32_t generation = 1) {
    harken::SelectionMemoryKeyV1 key{};
    key.world = 3;
    key.containerStableId = harkentest::ObjectId(100);
    key.programSemanticFingerprint = 200;
    key.memoryScope = scope;
    key.emitterSlot = scope == harken::MemoryScopeV1::Global ? UINT32_MAX : 0;
    key.emitterGeneration =
        scope == harken::MemoryScopeV1::Global ? 0 : generation;
    return key;
}

harken::SelectionPlayRequestV1 MemoryRequest(
    const MemoryProgram& fixture, std::uint64_t ticket,
    harken::SelectionMemoryKeyV1 key = MemoryKey()) {
    harken::SelectionPlayRequestV1 request{};
    request.key = key;
    request.program = &fixture.program;
    request.worldSemanticFingerprint = 300;
    request.rootStableId = harkentest::ObjectId(400);
    request.callerSeed = 500;
    request.acceptedPostOrdinal = ticket;
    request.ticket = ticket;
    return request;
}

TEST(CurrentSelection, CanonicalSeedBytesAndHashArePinned) {
    const auto input = GoldenInput();
    std::array<std::uint8_t, 300> bytes{};
    std::size_t size = 0;
    ASSERT_TRUE(harken::EncodeSelectionSeedV1(
        input, bytes.data(), bytes.size(), &size));
    ASSERT_EQ(bytes.size(), size);
    EXPECT_EQ(
        "530100001a04000001000000010800000807060504030201021000003da91a64"
        "b947456011121314151617180308000028272625242322210404000000000000"
        "0504000007000000060400000900000007080000000000000000000008100000"
        "3da91a64b9474560313233343536373809080000157c4a7fb979379e0a080000"
        "05000000000000000b010000010000000c010000640000000d04000001000000"
        "0e1000003da91a64b947456041424344454647480f1000003da91a64b9474560"
        "5152535455565758101000003da91a64b9474560616263646566676811080000"
        "7877767574737271120800008887868584838281130400000600000014040000"
        "0300000015040000040000001608000000000000000000001708000000000000"
        "000000001904000001000000",
        Hex(bytes.data(), size));

    std::uint64_t seed = 0;
    ASSERT_TRUE(harken::ComputeSelectionSeedV1(input, &seed));
    EXPECT_EQ(UINT64_C(0x7d2da2616b2b1428), seed);

    auto explicitDefaultSeed = input;
    explicitDefaultSeed.callerSeed = harken::SplitMix64::kDefaultSeed;
    std::uint64_t explicitSeed = 0;
    ASSERT_TRUE(
        harken::ComputeSelectionSeedV1(explicitDefaultSeed, &explicitSeed));
    EXPECT_EQ(seed, explicitSeed);
}

TEST(CurrentSelection, CanonicalSeedRefusalDoesNotMutateCallerBuffer) {
    const auto input = GoldenInput();
    std::array<std::uint8_t, 300> bytes{};
    bytes.fill(0xa5);
    std::size_t required = 0;
    EXPECT_FALSE(harken::EncodeSelectionSeedV1(
        input, bytes.data(), bytes.size() - 1, &required));
    EXPECT_EQ(bytes.size(), required);
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(),
                            [](std::uint8_t value) { return value == 0xa5; }));

    auto invalid = input;
    invalid.emitterGeneration = 0;
    std::uint64_t seed = 99;
    EXPECT_FALSE(harken::ComputeSelectionSeedV1(invalid, &seed));
    EXPECT_EQ(99u, seed);
}

TEST(CurrentSelection, RandomDrawUsesOneCarriedBoundedCall) {
    const harken::SelectionChildV1 children[] = {
        {2, 20, 2, 0},
        {5, 5, 0, 0},
        {9, 10, 3, 0},
    };
    harken::SplitMix64 stream(UINT64_C(0x53f5a85c0343d4fd));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::SelectRandomDrawV1(children, 3, nullptr, 0, &stream,
                                        &selected));
    EXPECT_EQ(9u, selected);
    EXPECT_EQ(UINT64_C(0xf22d2215828e5112), stream.State());

    const std::uint32_t recent[] = {2, 9};
    const std::uint64_t before = stream.State();
    EXPECT_EQ(harken::SelectionStatusV1::InvalidAvoid,
              harken::SelectRandomDrawV1(children, 3, recent, 2, &stream,
                                        &selected));
    EXPECT_EQ(before, stream.State());
}

TEST(CurrentSelection, RandomRotationCarriesOneStreamAndCrossCycleWindow) {
    const harken::SelectionChildV1 children[] = {
        {2, 20, 0, 1},
        {5, 5, 0, 2},
        {9, 10, 0, 3},
    };
    const std::uint32_t previousTail[] = {9};
    std::uint32_t scratch[3]{};
    std::uint32_t permutation[3]{};
    harken::SplitMix64 stream(UINT64_C(0x53f5a85c0343d4fd));
    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::BuildRandomRotationV1(
                  children, 3, previousTail, 1, &stream, scratch, 3,
                  permutation, 3));
    EXPECT_EQ((std::array<std::uint32_t, 3>{5, 2, 9}),
              (std::array<std::uint32_t, 3>{
                  permutation[0], permutation[1], permutation[2]}));
    EXPECT_EQ(UINT64_C(0x2e9c15888123493c), stream.State());

    std::fill(std::begin(scratch), std::end(scratch), 0xccccccccu);
    std::fill(std::begin(permutation), std::end(permutation), 0xddddddddu);
    EXPECT_EQ(harken::SelectionStatusV1::OutputCapacity,
              harken::BuildRandomRotationV1(
                  children, 3, previousTail, 1, &stream, scratch, 2,
                  permutation, 3));
    EXPECT_TRUE(std::all_of(std::begin(scratch), std::end(scratch),
                            [](std::uint32_t value) {
                                return value == 0xccccccccu;
                            }));
    EXPECT_TRUE(std::all_of(std::begin(permutation), std::end(permutation),
                            [](std::uint32_t value) {
                                return value == 0xddddddddu;
                            }));
}

TEST(CurrentSelection, SequenceUsesCompiledStableOrderAndEndPolicy) {
    const harken::SelectionChildV1 children[] = {
        {2, 20, 0, 0},
        {5, 5, 0, 0},
        {9, 10, 0, 0},
    };
    harken::SequenceSelectionV1 result{};
    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::SelectSequenceV1(
                  children, 3, 0, false,
                  harken::SequenceEndPolicyV1::Wrap, &result));
    EXPECT_EQ(5u, result.selectedChildOrdinal);
    EXPECT_EQ(1u, result.nextCursor);
    EXPECT_FALSE(result.stopped);

    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::SelectSequenceV1(
                  children, 3, 2, false,
                  harken::SequenceEndPolicyV1::StopAtEnd, &result));
    EXPECT_EQ(2u, result.selectedChildOrdinal);
    EXPECT_EQ(2u, result.nextCursor);
    EXPECT_TRUE(result.stopped);
    EXPECT_EQ(harken::SelectionStatusV1::SequenceComplete,
              harken::SelectSequenceV1(
                  children, 3, result.nextCursor, result.stopped,
                  harken::SequenceEndPolicyV1::StopAtEnd, &result));
}

TEST(CurrentSelectionMemory, HeadFailureRecomputesSuccessorFromCommittedState) {
    MemoryProgram program;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    const auto second = table.AppendPlay(MemoryRequest(program, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, second.status);
    const auto secondBefore = table.Delta(second.delta);
    EXPECT_TRUE(table.Delta(first.delta).head);
    EXPECT_FALSE(secondBefore.head);
    EXPECT_EQ(2u, secondBefore.selectionOrdinalAfter);
    EXPECT_EQ(2u, table.PendingCount());
    const auto pendingSnapshot = table.Snapshot(MemoryKey());
    ASSERT_TRUE(pendingSnapshot.found);
    EXPECT_EQ(0u, pendingSnapshot.selectionOrdinal);
    EXPECT_EQ(2u, pendingSnapshot.pendingCount);

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.FailHead(first.delta));
    EXPECT_FALSE(table.Delta(first.delta).live);
    const auto recomputed = table.Delta(second.delta);
    ASSERT_TRUE(recomputed.live);
    EXPECT_TRUE(recomputed.head);
    EXPECT_EQ(1u, recomputed.selectionOrdinalAfter);

    harken::SelectionMemoryTableV1 fresh;
    ASSERT_TRUE(fresh.Reset(MemoryConfig()));
    const auto expected = fresh.AppendPlay(MemoryRequest(program, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, expected.status);
    EXPECT_EQ(fresh.Delta(expected.delta).selectedChildOrdinal,
              recomputed.selectedChildOrdinal);

    EXPECT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(second.delta));
    const auto committed = table.Snapshot(MemoryKey());
    EXPECT_EQ(1u, committed.selectionOrdinal);
    EXPECT_EQ(0u, committed.pendingCount);
}

TEST(CurrentSelectionMemory,
     CancellingAMiddleDeltaRecomputesOnlyItsFollowingSuffix) {
    MemoryProgram program;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    const auto cancelled = table.AppendPlay(MemoryRequest(program, 2));
    const auto last = table.AppendPlay(MemoryRequest(program, 3));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, cancelled.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, last.status);
    const auto firstBefore = table.Delta(first.delta);
    const auto lastBefore = table.Delta(last.delta);
    ASSERT_TRUE(firstBefore.head);
    ASSERT_EQ(3u, lastBefore.selectionOrdinalAfter);

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CancelDelta(cancelled.delta));
    EXPECT_FALSE(table.Delta(cancelled.delta).live);
    EXPECT_TRUE(table.Delta(first.delta).head);
    const auto lastAfter = table.Delta(last.delta);
    ASSERT_TRUE(lastAfter.live);
    EXPECT_FALSE(lastAfter.head);
    EXPECT_EQ(2u, lastAfter.selectionOrdinalAfter);
    EXPECT_EQ(2u, table.PendingCount());

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(first.delta));
    EXPECT_TRUE(table.Delta(last.delta).head);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(last.delta));
    const auto committed = table.Snapshot(MemoryKey());
    EXPECT_EQ(2u, committed.selectionOrdinal);
    EXPECT_EQ(0u, committed.pendingCount);
}

TEST(CurrentSelectionMemory,
     CancellingTheOnlyUncommittedDeltaReleasesItsRow) {
    MemoryProgram program;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto only = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, only.status);
    EXPECT_EQ(1u, table.RowCount());
    EXPECT_EQ(1u, table.PendingCount());

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CancelDelta(only.delta));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_FALSE(table.Snapshot(MemoryKey()).found);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound,
              table.CancelDelta(only.delta));
}

TEST(CurrentSelectionMemory, PlayResetPlayFifoCommitsOnlyAtTheHead) {
    MemoryProgram program;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(first.delta));

    const auto reset = table.AppendReset(MemoryKey(), 2, 0);
    const auto later = table.AppendPlay(MemoryRequest(program, 3));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, reset.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, later.status);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead,
              table.CommitHead(later.delta));
    const auto laterView = table.Delta(later.delta);
    EXPECT_EQ(1u, laterView.resetEpochAfter);
    EXPECT_EQ(1u, laterView.selectionOrdinalAfter);

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(reset.delta));
    auto snapshot = table.Snapshot(MemoryKey());
    EXPECT_EQ(1u, snapshot.resetEpoch);
    EXPECT_EQ(0u, snapshot.selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(later.delta));
    snapshot = table.Snapshot(MemoryKey());
    EXPECT_EQ(1u, snapshot.resetEpoch);
    EXPECT_EQ(1u, snapshot.selectionOrdinal);

    auto missing = MemoryKey();
    missing.containerStableId = harkentest::ObjectId(999);
    missing.programSemanticFingerprint = 999;
    const auto missingReset = table.AppendReset(missing, 4, 0);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Ok, missingReset.status);
    EXPECT_TRUE(missingReset.idempotent);
    EXPECT_FALSE(missingReset.delta);
    EXPECT_FALSE(table.Snapshot(missing).found);
}

TEST(CurrentSelectionMemory, PreparedResetStaysHiddenUntilCommitOrCancel) {
    MemoryProgram program;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(first.delta));
    const std::uint32_t freeBefore = table.FreeDeltaCount();

    harken::PreparedSelectionResetV1 prepared;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PrepareReset(MemoryKey(), 2, 0, &prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(freeBefore - 1u, table.FreeDeltaCount());
    auto snapshot = table.Snapshot(MemoryKey());
    EXPECT_EQ(0u, snapshot.resetEpoch);
    EXPECT_EQ(1u, snapshot.selectionOrdinal);

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CancelPreparedReset(&prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(freeBefore, table.FreeDeltaCount());
    snapshot = table.Snapshot(MemoryKey());
    EXPECT_EQ(0u, snapshot.resetEpoch);
    EXPECT_EQ(1u, snapshot.selectionOrdinal);

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PrepareReset(MemoryKey(), 2, 0, &prepared));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitPreparedReset(&prepared, 2));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(freeBefore, table.FreeDeltaCount());
    snapshot = table.Snapshot(MemoryKey());
    EXPECT_EQ(1u, snapshot.resetEpoch);
    EXPECT_EQ(0u, snapshot.selectionOrdinal);

    auto missing = MemoryKey();
    missing.containerStableId = harkentest::ObjectId(999);
    missing.programSemanticFingerprint = 999;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PrepareReset(missing, 3, 0, &prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_TRUE(prepared.Idempotent());
    EXPECT_EQ(freeBefore, table.FreeDeltaCount());
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitPreparedReset(&prepared, 3));
    EXPECT_FALSE(table.Snapshot(missing).found);
}

TEST(CurrentSelectionMemory,
     PreparedPlayMovesToPendingWithoutApplyingFutureResetEarly) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(first.delta));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);

    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PrepareReset(MemoryKey(), 2, 0, &reset, 50));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitPreparedReset(&reset, 2));

    auto request = MemoryRequest(program, 3);
    request.executeFrame = 100;
    harken::PreparedSelectionPlayV1 prepared;
    std::uint32_t selectedChildOrdinal = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PreparePlay(
                  request, &prepared, &selectedChildOrdinal));
    EXPECT_EQ(5u, selectedChildOrdinal);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);

    harken::SelectionDeltaRefV1 pending{};
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.MovePreparedPlayToPending(
                  &prepared, request.ticket, &pending));
    ASSERT_TRUE(pending);
    const auto pendingView = table.Delta(pending);
    EXPECT_TRUE(pendingView.live);
    EXPECT_TRUE(pendingView.head);
    EXPECT_EQ(5u, pendingView.selectedChildOrdinal);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);

    table.ApplyDue(50);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(pending));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(0u, table.PendingCount());
}

TEST(CurrentSelectionMemory,
     PreparedResetCapacityIncludesHiddenReservationsButNotMissingNoOps) {
    MemoryProgram program;
    auto config = MemoryConfig();
    config.maxPendingDeltasTotal = 1;
    config.maxPendingGlobalPerRow = 1;
    config.maxPendingEmitterPerRow = 1;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(first.delta));

    harken::PreparedSelectionResetV1 reserved;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PrepareReset(MemoryKey(), 2, 0, &reserved));
    EXPECT_EQ(0u, table.FreeDeltaCount());

    harken::PreparedSelectionResetV1 refused;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Capacity,
              table.PrepareReset(MemoryKey(), 3, 0, &refused));
    EXPECT_FALSE(refused.Valid());

    auto missing = MemoryKey();
    missing.containerStableId = harkentest::ObjectId(999);
    missing.programSemanticFingerprint = 999;
    harken::PreparedSelectionResetV1 noOp;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PrepareReset(missing, 3, 0, &noOp));
    EXPECT_TRUE(noOp.Idempotent());
    EXPECT_EQ(0u, table.FreeDeltaCount());
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CancelPreparedReset(&noOp));

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CancelPreparedReset(&reserved));
    EXPECT_EQ(1u, table.FreeDeltaCount());
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).resetEpoch);
}

static void CheckPreparedPlayCancellationWithScheduledReset(std::uint64_t resetFrame,
    std::uint64_t resetTicket, std::uint32_t resetEvent) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(first.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.PrepareReset(MemoryKey(), resetTicket, resetEvent, &reset, resetFrame));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, resetTicket));
    auto earlierRequest = MemoryRequest(program, 3);
    earlierRequest.executeFrame = 320;
    harken::PreparedSelectionPlayV1 earlier;
    std::uint32_t earlierSelected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.PreparePlay(earlierRequest, &earlier, &earlierSelected));
    ASSERT_EQ(9u, earlierSelected);
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 640;
    request.eventIndex = 2;
    request.callerSeed = 87;
    harken::PreparedSelectionPlayV1 later;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &later, &selected));
    const bool resetApplies = resetFrame < request.executeFrame ||
        (resetFrame == request.executeFrame && (resetTicket < request.ticket ||
            (resetTicket == request.ticket && resetEvent < request.eventIndex)));
    EXPECT_EQ(resetApplies ? 5u : 2u, selected);
    auto probe = table.PreparedPlaySeedProbe(later);
    ASSERT_TRUE(probe.captured);
    EXPECT_EQ(resetApplies ? 1u : 0u, probe.resetEpoch);
    EXPECT_EQ(resetApplies ? 1u : 3u, probe.selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&earlier));
    probe = table.PreparedPlaySeedProbe(later);
    ASSERT_TRUE(probe.captured);
    EXPECT_EQ(resetApplies ? 1u : 0u, probe.resetEpoch);
    EXPECT_EQ(resetApplies ? 1u : 2u, probe.selectionOrdinal);
    EXPECT_EQ(request.callerSeed, probe.derivedCallerSeed);
    EXPECT_EQ(request.acceptedPostOrdinal, probe.acceptedPostOrdinal);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    harken::SelectionDeltaRefV1 pending{};
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.MovePreparedPlayToPending(&later, request.ticket, &pending));
    const auto delta = table.Delta(pending);
    ASSERT_TRUE(delta.live);
    EXPECT_EQ(request.ticket, delta.ticket);
    EXPECT_EQ(request.eventIndex, delta.eventIndex);
    EXPECT_EQ(resetApplies ? 5u : 9u, delta.selectedChildOrdinal);
    EXPECT_EQ(resetApplies ? 1u : 0u, delta.resetEpochAfter);
    EXPECT_EQ(resetApplies ? 1u : 2u, delta.selectionOrdinalAfter);
    if (resetApplies) table.ApplyDue(request.executeFrame);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(pending));
    EXPECT_EQ(resetApplies ? 1u : 0u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(resetApplies ? 1u : 2u, table.Snapshot(MemoryKey()).selectionOrdinal);
    table.ApplyDue(resetFrame);
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(MemoryConfig().maxPendingDeltasTotal, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PreparedPlayCancellationKeepsInterveningScheduledReset) {
    CheckPreparedPlayCancellationWithScheduledReset(640, 2, 0);
}

TEST(CurrentSelectionMemory, PreparedPlayCancellationDoesNotApplyFutureScheduledReset) {
    CheckPreparedPlayCancellationWithScheduledReset(960, 2, 0);
}

TEST(CurrentSelectionMemory, PreparedPlayCancellationKeepsEarlierSameTicketResetEvent) {
    CheckPreparedPlayCancellationWithScheduledReset(640, 4, 1);
}

TEST(CurrentSelectionMemory, PreparedPlayCancellationExcludesLaterSameTicketResetEvent) {
    CheckPreparedPlayCancellationWithScheduledReset(640, 4, 3);
}

TEST(CurrentSelectionMemory, PreparedPlayCancellationExcludesEqualSameTicketResetEvent) {
    CheckPreparedPlayCancellationWithScheduledReset(640, 4, 2);
}

TEST(CurrentSelectionMemory, PreparedPlayCancellationExcludesLaterSameFrameResetTicket) {
    CheckPreparedPlayCancellationWithScheduledReset(640, 5, 0);
}

static void CheckPreparedResetCancellationAtPlayFrame(bool scheduledReset) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(first.delta));
    if (scheduledReset) {
        harken::PreparedSelectionResetV1 scheduled;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            table.PrepareReset(MemoryKey(), 2, 0, &scheduled, 640));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&scheduled, 2));
    }
    harken::PreparedSelectionResetV1 cancelled;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.PrepareReset(MemoryKey(), 3, 0, &cancelled, scheduledReset ? 960u : 320u));
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 640;
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &play, &selected));
    EXPECT_EQ(5u, selected);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(play).resetEpoch);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(play).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&cancelled));
    const auto probe = table.PreparedPlaySeedProbe(play);
    ASSERT_TRUE(probe.captured);
    EXPECT_EQ(scheduledReset ? 1u : 0u, probe.resetEpoch);
    EXPECT_EQ(scheduledReset ? 1u : 2u, probe.selectionOrdinal);
    EXPECT_EQ(request.callerSeed, probe.derivedCallerSeed);
    EXPECT_EQ(request.acceptedPostOrdinal, probe.acceptedPostOrdinal);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    harken::SelectionDeltaRefV1 pending{};
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.MovePreparedPlayToPending(&play, request.ticket, &pending));
    EXPECT_EQ(scheduledReset ? 5u : 9u, table.Delta(pending).selectedChildOrdinal);
    if (scheduledReset) table.ApplyDue(request.executeFrame);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(pending));
    EXPECT_EQ(scheduledReset ? 1u : 0u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(scheduledReset ? 1u : 2u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(MemoryConfig().maxPendingDeltasTotal, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PreparedResetCancellationKeepsApplicableScheduledReset) {
    CheckPreparedResetCancellationAtPlayFrame(true);
}

TEST(CurrentSelectionMemory, PreparedResetCancellationRestoresCommittedSelectionPrefix) {
    CheckPreparedResetCancellationAtPlayFrame(false);
}

TEST(CurrentSelectionMemory, PreparedResetAccumulatesEarlierScheduledResetEpoch) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(first.delta));
    for (unsigned index = 0; index < 2; ++index) {
        harken::PreparedSelectionResetV1 reset;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            table.PrepareReset(MemoryKey(), 2u + index, 0, &reset, 640u + 320u * index));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, 2u + index));
    }
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 640;
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &play, &selected));
    EXPECT_EQ(5u, selected);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(play).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
    table.ApplyDue(640);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    table.ApplyDue(960);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(MemoryConfig().maxPendingDeltasTotal, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PreparedResetsCommitInProvisionalOrder) {
    MemoryProgram program;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(first.delta));

    harken::PreparedSelectionResetV1 earlier;
    harken::PreparedSelectionResetV1 later;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PrepareReset(MemoryKey(), 2, 0, &earlier));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PrepareReset(MemoryKey(), 2, 1, &later));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead,
              table.CommitPreparedReset(&later, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitPreparedReset(&earlier, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitPreparedReset(&later, 2));
    const auto snapshot = table.Snapshot(MemoryKey());
    EXPECT_EQ(2u, snapshot.resetEpoch);
    EXPECT_EQ(0u, snapshot.selectionOrdinal);
}

static void CheckScheduledResetsWithReverseFrameOrder(bool sameTicket) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(first.delta));
    for (unsigned index = 0; index < 2; ++index) {
        const auto ticket = sameTicket ? 2u : 2u + index;
        const auto event = sameTicket ? index : 0u;
        harken::PreparedSelectionResetV1 reset;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            table.PrepareReset(MemoryKey(), ticket, event, &reset, 960u - index * 320u));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            table.CommitPreparedReset(&reset, ticket));
    }
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 1280;
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.PreparePlay(request, &play, &selected));
    EXPECT_EQ(5u, selected);
    const auto probe = table.PreparedPlaySeedProbe(play);
    ASSERT_TRUE(probe.captured);
    EXPECT_EQ(2u, probe.resetEpoch);
    EXPECT_EQ(1u, probe.selectionOrdinal);
    EXPECT_EQ(request.callerSeed, probe.derivedCallerSeed);
    EXPECT_EQ(request.acceptedPostOrdinal, probe.acceptedPostOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
    table.ApplyDue(640);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    table.ApplyDue(960);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(MemoryConfig().maxPendingDeltasTotal, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ScheduledResetsAccumulateWithReverseAcceptedFrames) {
    CheckScheduledResetsWithReverseFrameOrder(false);
}

TEST(CurrentSelectionMemory, ScheduledResetsAccumulateWithReverseFramesWithinOneTicket) {
    CheckScheduledResetsWithReverseFrameOrder(true);
}

static harken::SelectionMemoryStatusV1 PrepareMemoryBatchMember(harken::SelectionMemoryTableV1& table,
    const MemoryProgram& program, harken::SelectionMemoryKeyV1 key, std::uint64_t ticket,
    std::uint32_t event, std::uint64_t frame, harken::PreparedSelectionPlayV1& token) {
    auto request = MemoryRequest(program, ticket, key);
    request.eventIndex = event;
    request.executeFrame = frame;
    request.callerSeed = 700u + event;
    request.acceptedPostOrdinal = 87;
    std::uint32_t selected = 0;
    return table.PreparePlay(request, &token, &selected);
}

static harken::SelectionMemoryStatusV1 PrepareNativeMemoryMember(harken::SelectionMemoryTableV1& table,
    const MemoryProgram& program, harken::SelectionMemoryKeyV1 key, std::uint64_t ticket,
    std::uint32_t event, std::uint64_t frame, harken::PreparedSelectionPlayV1& token) {
    auto request = MemoryRequest(program, ticket, key);
    request.eventIndex = event;
    request.executeFrame = frame;
    request.callerSeed = 700u + event;
    request.acceptedPostOrdinal = 87;
    std::uint32_t selected = 0;
    return table.PreparePlayForPendingBatch(request, &token, &selected);
}

static harken::SelectionMemoryStatusV1 MoveNativeMemoryMember(harken::SelectionMemoryTableV1& table,
    harken::PreparedSelectionPlayV1& token, std::uint64_t ticket,
    harken::PendingSelectionBatchV1& batch) {
    harken::PreparedSelectionPlayV1* tokens[] = {&token};
    return table.MovePreparedPlaysToPending(tokens, 1, ticket, &batch);
}

TEST(CurrentSelectionMemory, NativeFollowerBatchesRebaseAfterFirstCancellationWithReverseFrames) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 batches[3];
    for (std::uint32_t index = 0; index < 3; ++index) {
        harken::PreparedSelectionPlayV1 token;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            PrepareNativeMemoryMember(table, program, MemoryKey(), index + 1u, 0, 960u - index * 320u, token));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, token, index + 1u, batches[index]));
    }
    EXPECT_EQ(5u, table.ReadPendingBatchMember(batches[0], 0).delta.selectedChildOrdinal);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(batches[1], 0).delta.selectedChildOrdinal);
    EXPECT_EQ(2u, table.ReadPendingBatchMember(batches[2], 0).delta.selectedChildOrdinal);
    const auto oldSecond = table.ReadPendingBatch(batches[1]);
    const auto oldThird = table.ReadPendingBatch(batches[2]);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batches[0]));
    EXPECT_EQ(5u, table.ReadPendingBatchMember(batches[1], 0).delta.selectedChildOrdinal);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(batches[2], 0).delta.selectedChildOrdinal);
    EXPECT_EQ(2u, table.ReadPendingBatchMember(batches[1], 0).delta.ticket);
    EXPECT_EQ(3u, table.ReadPendingBatchMember(batches[2], 0).delta.ticket);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPendingBatch(&batches[1], oldSecond.revision));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPendingBatch(&batches[2], oldThird.revision));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead,
        table.CommitPendingBatch(&batches[2], table.ReadPendingBatch(batches[2]).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&batches[1], table.ReadPendingBatch(batches[1]).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&batches[2], table.ReadPendingBatch(batches[2]).revision));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).sequenceCursor);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, NativeHiddenFollowersMoveWithCurrentOverlayAfterPredecessorCancellation) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 first, second, third;
    harken::PendingSelectionBatchV1 firstBatch, secondBatch, thirdBatch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 960, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, first, 1, firstBatch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 0, 640, second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 3, 0, 320, third));
    EXPECT_EQ(3u, table.PreparedPlaySeedProbe(third).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&firstBatch));
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(second).selectionOrdinal);
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(third).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, second, 2, secondBatch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, third, 3, thirdBatch));
    EXPECT_EQ(5u, table.ReadPendingBatchMember(secondBatch, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(thirdBatch, 0).delta.selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&secondBatch, table.ReadPendingBatch(secondBatch).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&thirdBatch));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckNativeFollowerAfterOrdinaryWithdrawal(bool failHead) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    harken::PreparedSelectionPlayV1 second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 0, 640, second));
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, second, 2, batch));
    const auto old = table.ReadPendingBatch(batch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, failHead ? table.FailHead(first.delta) : table.CancelDelta(first.delta));
    EXPECT_EQ(5u, table.ReadPendingBatchMember(batch, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CanCommitPendingBatch(batch, old.revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, NativeFollowerRebasesAfterOrdinaryPredecessorCancel) { CheckNativeFollowerAfterOrdinaryWithdrawal(false); }
TEST(CurrentSelectionMemory, NativeFollowerRebasesAfterOrdinaryPredecessorFailure) { CheckNativeFollowerAfterOrdinaryWithdrawal(true); }

TEST(CurrentSelectionMemory, NativeSameTicketEventsKeepContiguousPrefixBehindEarlierProducer) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto predecessor = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, predecessor.status);
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 4, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 5, 640, second));
    harken::PreparedSelectionPlayV1* reversed[] = {&second, &first};
    harken::PendingSelectionBatchV1 batch;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.MovePreparedPlaysToPending(reversed, 2, 2, &batch));
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_EQ(1u, table.PendingCount());
    harken::PreparedSelectionPlayV1* ordered[] = {&first, &second};
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(ordered, 2, 2, &batch));
    EXPECT_EQ(9u, table.ReadPendingBatchMember(batch, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(2u, table.ReadPendingBatchMember(batch, 1).delta.selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(predecessor.delta));
    EXPECT_EQ(5u, table.ReadPendingBatchMember(batch, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(batch, 1).delta.selectedChildOrdinal);
    EXPECT_EQ(4u, table.ReadPendingBatchMember(batch, 0).delta.eventIndex);
    EXPECT_EQ(5u, table.ReadPendingBatchMember(batch, 1).delta.eventIndex);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, NativeCrossKeyBatchWaitsForGlobalPredecessorWithoutCommittingEmitter) {
    MemoryProgram global(harken::ContainerModeV1::Sequence), emitter(harken::ContainerModeV1::Sequence);
    global.program.memoryScope = harken::MemoryScopeV1::Global;
    const auto globalKey = MemoryKey(harken::MemoryScopeV1::Global);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto predecessor = table.AppendPlay(MemoryRequest(global, 1, globalKey));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, predecessor.status);
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, global, globalKey, 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, emitter, MemoryKey(), 2, 1, 640, second));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(predecessor.delta));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    EXPECT_EQ(1u, table.Snapshot(globalKey).selectionOrdinal);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, NativeHiddenPreparationRejectsLegacyPlayAndResetUntilRecycled) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 native, legacy;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, native));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, legacy));
    harken::PreparedSelectionResetV1 reset;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.PrepareReset(MemoryKey(), 2, 0, &reset, 640));
    EXPECT_FALSE(legacy.Valid());
    EXPECT_FALSE(reset.Valid());
    EXPECT_EQ(7u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&native));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, legacy));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlay(&legacy, 2));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

static void CheckNativeCannotFollowLegacyHidden(bool prepareReset) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto committed = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, committed.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(committed.delta));
    harken::PreparedSelectionPlayV1 legacy, native;
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, prepareReset ? table.PrepareReset(MemoryKey(), 2, 0, &reset, 640)
        : PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, legacy));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, PrepareNativeMemoryMember(table, program, MemoryKey(), 3, 0, 960, native));
    EXPECT_FALSE(native.Valid());
    EXPECT_EQ(7u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, prepareReset ? table.CancelPreparedReset(&reset) : table.CancelPreparedPlay(&legacy));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 3, 0, 960, native));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&native));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, NativeHiddenPreparationRejectsLegacyPlayPredecessor) { CheckNativeCannotFollowLegacyHidden(false); }
TEST(CurrentSelectionMemory, NativeHiddenPreparationRejectsLegacyResetPredecessor) { CheckNativeCannotFollowLegacyHidden(true); }

TEST(CurrentSelectionMemory, NativePreparedTokenCannotUseSingleItemOrTransactionalConsumers) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 native;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, native));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPreparedPlay(&native, 1));
    harken::SelectionDeltaRefV1 ref;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.MovePreparedPlayToPending(&native, 1, &ref));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPreparedPlayTransactional(&native, 1));
    EXPECT_TRUE(native.Valid());
    EXPECT_FALSE(native.Committed());
    EXPECT_FALSE(ref);
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&native));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, NativePreparationRejectsUnsettledTransactionWithoutConsumingCapacity) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 transaction, native;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 1, 0, 320, transaction));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlayTransactional(&transaction, 1));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 0, 640, native));
    EXPECT_FALSE(native.Valid());
    EXPECT_EQ(7u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.RollbackCommittedPlay(&transaction));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 0, 640, native));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&native));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CommitSelectionAndScheduledReset(harken::SelectionMemoryTableV1& table, const MemoryProgram& program) {
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &reset, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, 2));
}

TEST(CurrentSelectionMemory, DueScheduledResetRebasesOrdinaryPendingInsteadOfRewindingCommittedEpoch) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    CommitSelectionAndScheduledReset(table, program);
    auto request = MemoryRequest(program, 3);
    request.executeFrame = 320;
    const auto pending = table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, pending.status);
    EXPECT_EQ(9u, table.Delta(pending.delta).selectedChildOrdinal);
    table.ApplyDue(640);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Delta(pending.delta).resetEpochAfter);
    EXPECT_EQ(1u, table.Delta(pending.delta).selectionOrdinalAfter);
    EXPECT_EQ(5u, table.Delta(pending.delta).selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(pending.delta));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, DueScheduledResetRebasesNativeHeadAndFollowerAndInvalidatesOldRevision) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    CommitSelectionAndScheduledReset(table, program);
    harken::PreparedSelectionPlayV1 first, second;
    harken::PendingSelectionBatchV1 firstBatch, secondBatch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 3, 0, 320, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, first, 3, firstBatch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 4, 0, 1280, second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, second, 4, secondBatch));
    EXPECT_EQ(9u, table.ReadPendingBatchMember(firstBatch, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(5u, table.ReadPendingBatchMember(secondBatch, 0).delta.selectedChildOrdinal);
    const auto oldFirst = table.ReadPendingBatch(firstBatch);
    const auto oldSecond = table.ReadPendingBatch(secondBatch);
    table.ApplyDue(640);
    EXPECT_EQ(5u, table.ReadPendingBatchMember(firstBatch, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(secondBatch, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(1u, table.ReadPendingBatchMember(firstBatch, 0).seedProbe.resetEpoch);
    EXPECT_EQ(1u, table.ReadPendingBatchMember(secondBatch, 0).seedProbe.resetEpoch);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPendingBatch(&firstBatch, oldFirst.revision));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPendingBatch(&secondBatch, oldSecond.revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&firstBatch, table.ReadPendingBatch(firstBatch).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&secondBatch));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, DueScheduledResetRebasesNativeHiddenPreparedChainBeforeMove) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    CommitSelectionAndScheduledReset(table, program);
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 3, 0, 320, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 4, 0, 1280, second));
    table.ApplyDue(640);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(first).resetEpoch);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(first).selectionOrdinal);
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(second).selectionOrdinal);
    harken::PendingSelectionBatchV1 firstBatch, secondBatch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, first, 3, firstBatch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, second, 4, secondBatch));
    EXPECT_EQ(5u, table.ReadPendingBatchMember(firstBatch, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(secondBatch, 0).delta.selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&firstBatch, table.ReadPendingBatch(firstBatch).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&secondBatch, table.ReadPendingBatch(secondBatch).revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, PendingResetAcceptKeepsEarlierWaitingPlayHistoryUnchanged) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 first;
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, first, 1, batch));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.PreparePendingReset(MemoryKey(), 2, 0, &reset));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&reset, 2));
    table.ApplyDue(1280);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).resetEpoch);
    auto view = table.ReadPendingBatch(batch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, view.revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    table.ApplyDue(1280);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetCanBeReservedBeforeSameActionPlayResetPlay) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto committed = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, committed.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(committed.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 1, &reset, 640));
    harken::PreparedSelectionPlayV1 before, after;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 0, 640, before));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 2, 640, after));
    EXPECT_EQ(0u, table.PreparedPlaySeedProbe(before).resetEpoch);
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(before).selectionOrdinal);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(after).resetEpoch);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(after).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&reset, 2));
    harken::PendingSelectionBatchV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, before, 2, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, after, 2, second));
    EXPECT_EQ(9u, table.ReadPendingBatchMember(first, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(5u, table.ReadPendingBatchMember(second, 0).delta.selectedChildOrdinal);
    auto view = table.ReadPendingBatch(first);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&first, view.revision));
    view = table.ReadPendingBatch(second);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&second, view.revision));
    table.ApplyDue(1280);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetCancellationRecomputesSameActionHiddenPlay) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto committed = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, committed.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(committed.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &reset, 640));
    harken::PreparedSelectionPlayV1 after;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 1, 640, after));
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(after).resetEpoch);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(after).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&reset));
    EXPECT_EQ(0u, table.PreparedPlaySeedProbe(after).resetEpoch);
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(after).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&after));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingFutureResetDoesNotChangeEarlierHiddenPlay) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto committed = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, committed.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(committed.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &reset, 1280));
    harken::PreparedSelectionPlayV1 before;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 1, 640, before));
    EXPECT_EQ(0u, table.PreparedPlaySeedProbe(before).resetEpoch);
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(before).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&reset));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&before));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetAcceptRejectsLegacyReservationWithoutConsumingIt) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto committed = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, committed.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(committed.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &reset, 640));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.AcceptPreparedPendingReset(&reset, 2));
    EXPECT_TRUE(reset.Valid());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&reset));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).resetEpoch);
}

TEST(CurrentSelectionMemory, LegacyResetConsumerRejectsPendingReservationWithoutConsumingIt) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto committed = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, committed.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(committed.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &reset, 640));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPreparedReset(&reset, 2));
    EXPECT_TRUE(reset.Valid());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&reset));
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).resetEpoch);
}

TEST(CurrentSelectionMemory, PendingResetMissingRowCanStillSeparateSameActionPlays) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 1, 1, &reset, 640));
    harken::PreparedSelectionPlayV1 before, after;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, before));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 2, 640, after));
    bool noOp = true;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CanAcceptPreparedPendingReset(reset, 1, &noOp));
    EXPECT_FALSE(noOp);
    EXPECT_EQ(0u, table.PreparedPlaySeedProbe(before).resetEpoch);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(after).resetEpoch);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(after).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&reset, 1));
    harken::PendingSelectionBatchV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, before, 1, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, after, 1, second));
    EXPECT_EQ(5u, table.ReadPendingBatchMember(first, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(5u, table.ReadPendingBatchMember(second, 0).delta.selectedChildOrdinal);
    auto view = table.ReadPendingBatch(first);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&first, view.revision));
    view = table.ReadPendingBatch(second);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&second, view.revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetWithOnlyLaterVirginPlayIsFinallyNoOp) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 1, 0, &reset, 640));
    harken::PreparedSelectionPlayV1 after;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 1, 640, after));
    bool noOp = false;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CanAcceptPreparedPendingReset(reset, 1, &noOp));
    EXPECT_TRUE(noOp);
    EXPECT_EQ(0u, table.PreparedPlaySeedProbe(after).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&reset, 1));
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, after, 1, batch));
    auto view = table.ReadPendingBatch(batch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, view.revision));
    table.ApplyDue(1280);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetEarlierFrameAcceptedAfterConsumedFrontierStillRunsOnce) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto committed = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, committed.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(committed.delta));
    harken::PreparedSelectionResetV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &first, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&first, 2));
    table.ApplyDue(640);
    ASSERT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 3, 0, &second, 320));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&second, 3));
    harken::PreparedSelectionPlayV1 after;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 4, 0, 1280, after));
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(after).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&after));
    table.ApplyDue(1280);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    table.ApplyDue(1280);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetImmediateBlockerDoesNotDelayApplicablePositiveReset) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 play;
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 1000, play));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, play, 1, batch));
    harken::PreparedSelectionResetV1 immediate, positive;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &immediate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&immediate, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 3, 0, &positive, 100));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&positive, 3));
    table.ApplyDue(100);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(1u, table.ReadPendingBatchMember(batch, 0).seedProbe.resetEpoch);
    auto view = table.ReadPendingBatch(batch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, view.revision));
    table.ApplyDue(2000);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetPrivateDomainRejectsAnotherTicketWithoutLosingCapacity) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto committed = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, committed.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(committed.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &reset, 640));
    const auto free = table.FreeDeltaCount();
    harken::PreparedSelectionPlayV1 other;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 3, 0, 1280, other));
    EXPECT_FALSE(other.Valid());
    EXPECT_EQ(free, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&reset));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 3, 0, 1280, other));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&other));
}

TEST(CurrentSelectionMemory, PendingResetMissingRowReservationRetiresWithEmitter) {
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 1, 0, &reset, 640));
    EXPECT_EQ(7u, table.FreeDeltaCount());
    table.RetireEmitter(0, 1);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.AcceptPreparedPendingReset(&reset, 1));
    EXPECT_EQ(8u, table.FreeDeltaCount());
    EXPECT_EQ(0u, table.RowCount());
}

TEST(CurrentSelectionMemory, PendingResetLegacyPlayUsesActualResetAndConsumesItOnce) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &reset, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&reset, 2));
    auto request = MemoryRequest(program, 3);
    request.executeFrame = 1000;
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &play, &selected));
    EXPECT_EQ(5u, selected);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(play).resetEpoch);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(play).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlay(&play, 3));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    table.ApplyDue(2000);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetLegacyImmediateResetConsumesNativeJournalOnce) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 native, legacy;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &native));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&native, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 3, 0, &legacy));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&legacy, 3));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(8u, table.FreeDeltaCount());
    table.ApplyDue(2000);
    table.ApplyDue(2000);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, PendingResetLegacyScheduledResetKeepsBothOperations) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 native, legacy;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &native, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&native, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 3, 0, &legacy, 1280));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&legacy, 3));
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 1600;
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &play, &selected));
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(play).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
    table.ApplyDue(640);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    table.ApplyDue(1280);
    table.ApplyDue(1600);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetLegacyRollbackRetainsOperationUntilActualConsumption) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &reset, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&reset, 2));
    auto request = MemoryRequest(program, 3);
    request.executeFrame = 1280;
    harken::PreparedSelectionPlayV1 transaction;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &transaction, &selected));
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(transaction).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlayTransactional(&transaction, 3));
    table.ApplyDue(2000);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(6u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.RollbackCommittedPlay(&transaction));
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    table.ApplyDue(2000);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetRejectsAlreadyPreparedPlayWithSameEvent) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 play;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, play));
    harken::PreparedSelectionResetV1 reset;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.PreparePendingReset(MemoryKey(), 1, 0, &reset, 640));
    EXPECT_FALSE(reset.Valid());
    EXPECT_EQ(7u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
}

TEST(CurrentSelectionMemory, PendingResetRejectsLaterPreparedPlayWithSameEvent) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 1, 0, &reset, 640));
    harken::PreparedSelectionPlayV1 play;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, play));
    EXPECT_FALSE(play.Valid());
    EXPECT_EQ(7u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&reset));
}

TEST(CurrentSelectionMemory, PendingResetPrivateImmediateAndPositiveFrontiersAreIndependent) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 positive, immediate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &positive, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 1, &immediate));
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 2, 1280, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 3, 1280, second));
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(first).resetEpoch);
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(second).resetEpoch);
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(second).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&positive, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&immediate, 2));
    harken::PreparedSelectionPlayV1* members[]{&first, &second};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(members, 2, 2, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    table.ApplyDue(2000);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetPrivateReservationCountsAgainstMissingKeyCapacity) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    auto config = MemoryConfig();
    config.maxPendingEmitterPerRow = 1;
    ASSERT_TRUE(table.Reset(config));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 1, 1, &reset));
    harken::PreparedSelectionPlayV1 play;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Capacity,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, play));
    EXPECT_FALSE(play.Valid());
    EXPECT_EQ(7u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&reset));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, play));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetPrivateWorldRetirementAndNewArenaRejectOldTokens) {
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionResetV1 retired;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 1, 0, &retired));
    table.RetireWorld(MemoryKey().world);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CanAcceptPreparedPendingReset(retired, 1));
    EXPECT_EQ(8u, table.FreeDeltaCount());
    harken::PreparedSelectionResetV1 old, current;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &old));
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 3, 0, &current));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.AcceptPreparedPendingReset(&old, 2));
    EXPECT_EQ(7u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&current));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetAcceptedJournalKeepsKeyAfterOriginalWaitingPlayCancels) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 play;
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, play));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, play, 1, batch));
    harken::PreparedSelectionResetV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&first, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 3, 0, &second));
    bool noOp = true;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CanAcceptPreparedPendingReset(second, 3, &noOp));
    EXPECT_FALSE(noOp);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&second, 3));
    table.ApplyDue(2000);
    table.ApplyDue(2000);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckPendingResetLateMixedAdmission(bool existingNative) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    auto config = MemoryConfig();
    config.maxPendingEmitterPerRow = 8;
    ASSERT_TRUE(table.Reset(config));
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 first, legacy, late;
    if (existingNative) {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 2, 0, &first, 640));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&first, 2));
    }
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.PrepareReset(MemoryKey(), 3, 0, &legacy, existingNative ? 1280u : 640u));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&legacy, 3));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.PreparePendingReset(MemoryKey(), 4, 0, &late, existingNative ? 960u : 320u));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&late, 4));
    const auto expectedEpoch = existingNative ? 3u : 2u;
    auto request = MemoryRequest(program, 5);
    request.executeFrame = 1600;
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &play, &selected));
    EXPECT_EQ(expectedEpoch, table.PreparedPlaySeedProbe(play).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareNativeMemoryMember(table, program, MemoryKey(), 5, 0, 1600, play));
    EXPECT_EQ(expectedEpoch, table.PreparedPlaySeedProbe(play).resetEpoch);
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, play, 5, batch));
    EXPECT_EQ(expectedEpoch, table.ReadPendingBatchMember(batch, 0).seedProbe.resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    table.ApplyDue(2000);
    table.ApplyDue(2000);
    EXPECT_EQ(expectedEpoch, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingResetLateAdmissionAfterLegacySchedulePreservesBothEpochs) {
    CheckPendingResetLateMixedAdmission(false);
}

TEST(CurrentSelectionMemory, PendingResetLateAdmissionBetweenMixedSchedulesPreservesAllEpochs) {
    CheckPendingResetLateMixedAdmission(true);
}

static void CheckNativeRandomFollowerReference(harken::RandomDisciplineV1 discipline) {
    MemoryProgram program(harken::ContainerModeV1::Random, discipline);
    harken::SelectionMemoryTableV1 table, reference;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    ASSERT_TRUE(reference.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 batches[3];
    harken::SelectionPathNodeV1 originalPaths[3];
    harken::SelectionPlayRequestV1 requests[3];
    auto callerPath = GoldenInput().path[0];
    for (std::uint32_t index = 0; index < 3; ++index) {
        callerPath.childOrdinal = index + 4u;
        originalPaths[index] = callerPath;
        auto request = MemoryRequest(program, index + 1u);
        request.executeFrame = 960u - index * 320u;
        request.eventIndex = index + 5u;
        request.callerSeed = 705u + index;
        request.acceptedPostOrdinal = 87u + index;
        request.pathCount = 1;
        request.path = &callerPath;
        request.occurrenceOrdinal = 3u + index;
        request.loopIteration = 4u + index;
        requests[index] = request;
        requests[index].path = &originalPaths[index];
        harken::PreparedSelectionPlayV1 token;
        std::uint32_t selected = 0;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(request, &token, &selected));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, token, request.ticket, batches[index]));
    }
    callerPath = {};
    const auto oldSecond = table.ReadPendingBatchMember(batches[1], 0);
    const auto oldThird = table.ReadPendingBatchMember(batches[2], 0);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batches[0]));
    harken::SelectionDeltaRefV1 referenceDeltas[2];
    for (std::uint32_t index = 1; index < 3; ++index) {
        const auto expected = reference.AppendPlay(requests[index]);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, expected.status);
        referenceDeltas[index - 1] = expected.delta;
        const auto actual = table.ReadPendingBatchMember(batches[index], 0);
        EXPECT_EQ(reference.Delta(expected.delta).selectedChildOrdinal, actual.delta.selectedChildOrdinal);
        EXPECT_EQ(requests[index].ticket, actual.delta.ticket);
        EXPECT_EQ(requests[index].eventIndex, actual.delta.eventIndex);
        EXPECT_EQ(requests[index].callerSeed, actual.seedProbe.derivedCallerSeed);
        EXPECT_EQ(requests[index].acceptedPostOrdinal, actual.seedProbe.acceptedPostOrdinal);
        EXPECT_EQ(index, actual.seedProbe.selectionOrdinal);
        EXPECT_EQ(0u, actual.seedProbe.resetEpoch);
        const auto& old = index == 1 ? oldSecond : oldThird;
        EXPECT_EQ(old.ref.slot, actual.ref.slot);
        EXPECT_EQ(old.ref.generation, actual.ref.generation);
        EXPECT_EQ(old.ref.lifetime, actual.ref.lifetime);
        EXPECT_NE(old.revision, actual.revision);
    }
    for (std::uint32_t index = 1; index < 3; ++index) {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            table.CommitPendingBatch(&batches[index], table.ReadPendingBatch(batches[index]).revision));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, reference.CommitHead(referenceDeltas[index - 1]));
    }
    EXPECT_EQ(reference.Snapshot(MemoryKey()).selectionOrdinal, table.Snapshot(MemoryKey()).selectionOrdinal);
    std::uint32_t actual[3]{}, expected[3]{};
    EXPECT_EQ(reference.CopyRecent(MemoryKey(), expected, 3), table.CopyRecent(MemoryKey(), actual, 3));
    EXPECT_TRUE(std::equal(std::begin(expected), std::end(expected), std::begin(actual)));
    std::fill(std::begin(actual), std::end(actual), 0u);
    std::fill(std::begin(expected), std::end(expected), 0u);
    EXPECT_EQ(reference.CopyPermutation(MemoryKey(), expected, 3), table.CopyPermutation(MemoryKey(), actual, 3));
    EXPECT_TRUE(std::equal(std::begin(expected), std::end(expected), std::begin(actual)));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, NativeRandomDrawFollowerRetainsAuthenticSeedAndOwnedPath) {
    CheckNativeRandomFollowerReference(harken::RandomDisciplineV1::Draw);
}

TEST(CurrentSelectionMemory, NativeRandomRotationFollowerRetainsAuthenticSeedAndOwnedPath) {
    CheckNativeRandomFollowerReference(harken::RandomDisciplineV1::Rotation);
}

TEST(CurrentSelectionMemory, NativeFollowerCapacityIncludesPendingPreparedAndScheduledOwners) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    auto config = MemoryConfig();
    config.maxPendingEmitterPerRow = 3;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    CommitSelectionAndScheduledReset(table, program);
    harken::PreparedSelectionPlayV1 first, second, refused;
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 3, 0, 1280, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, first, 3, batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 4, 0, 1280, second));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Capacity, PrepareNativeMemoryMember(table, program, MemoryKey(), 5, 0, 1280, refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(1u, table.PendingCount());
    EXPECT_EQ(5u, table.FreeDeltaCount());
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, PrepareNativeMemoryMember(table, program, MemoryKey(), 5, 0, 1280, second));
    EXPECT_TRUE(second.Valid());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&second));
    table.ApplyDue(640);
    EXPECT_EQ(8u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 5, 0, 1280, refused));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&refused));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, NativeFollowerLateCrossKeyPreflightFailurePreservesAllOwners) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto predecessor = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, predecessor.status);
    auto otherKey = MemoryKey();
    otherKey.emitterSlot = 1;
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, otherKey, 2, 1, 960, second));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    EXPECT_FALSE(batch.Valid());
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_EQ(1u, table.PendingCount());
    EXPECT_EQ(5u, table.FreeDeltaCount());
    EXPECT_EQ(8u, table.FreePendingBatchCount());
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(first).selectionOrdinal);
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(second).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(predecessor.delta));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, NativeEmitterFollowersRemainIsolatedWhenOtherEmitterIsCancelled) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto otherKey = MemoryKey();
    otherKey.emitterSlot = 1;
    harken::PendingSelectionBatchV1 first, second, other;
    harken::PreparedSelectionPlayV1 tokenA, tokenB, tokenOther;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 1, 0, 640, tokenA));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, tokenA, 1, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, MemoryKey(), 2, 0, 640, tokenB));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, tokenB, 2, second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareNativeMemoryMember(table, program, otherKey, 3, 0, 640, tokenOther));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, MoveNativeMemoryMember(table, tokenOther, 3, other));
    const auto otherRevision = table.ReadPendingBatch(other).revision;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&first));
    EXPECT_EQ(5u, table.ReadPendingBatchMember(second, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(5u, table.ReadPendingBatchMember(other, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(otherRevision, table.ReadPendingBatch(other).revision);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&other, otherRevision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&second, table.ReadPendingBatch(second).revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(1u, table.Snapshot(otherKey).selectionOrdinal);
}

TEST(CurrentSelectionMemory, PendingBatchMovesTwoGlobalSelectionsAndCommitsOnce) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    program.program.memoryScope = harken::MemoryScopeV1::Global;
    const auto key = MemoryKey(harken::MemoryScopeV1::Global);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, key, 3, 4, 1280, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, key, 3, 5, 1280, second));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 2, 3, &batch));
    EXPECT_FALSE(first.Valid());
    EXPECT_FALSE(second.Valid());
    const auto view = table.ReadPendingBatch(batch);
    ASSERT_TRUE(view.live);
    EXPECT_EQ(2u, view.memberCount);
    EXPECT_EQ(0u, table.Snapshot(key).selectionOrdinal);
    const auto a = table.ReadPendingBatchMember(batch, 0);
    const auto b = table.ReadPendingBatchMember(batch, 1);
    EXPECT_EQ(5u, a.delta.selectedChildOrdinal);
    EXPECT_EQ(9u, b.delta.selectedChildOrdinal);
    EXPECT_EQ(3u, b.delta.ticket);
    EXPECT_EQ(5u, b.delta.eventIndex);
    EXPECT_EQ(705u, b.seedProbe.derivedCallerSeed);
    EXPECT_EQ(87u, b.seedProbe.acceptedPostOrdinal);
    EXPECT_EQ(2u, b.seedProbe.selectionOrdinal);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitHead(a.ref));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.FailHead(a.ref));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CancelDelta(b.ref));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPendingBatch(&batch, view.revision + 1));
    EXPECT_EQ(0u, table.Snapshot(key).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, view.revision));
    EXPECT_FALSE(batch.Valid());
    EXPECT_EQ(2u, table.Snapshot(key).selectionOrdinal);
    EXPECT_EQ(2u, table.Snapshot(key).sequenceCursor);
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(MemoryConfig().maxPendingDeltasTotal, table.FreeDeltaCount());
    EXPECT_EQ(MemoryConfig().maxPendingDeltasTotal, table.FreePendingBatchCount());
}

TEST(CurrentSelectionMemory, PendingBatchPreflightFailurePreservesEveryPreparedMember) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table, foreign;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    ASSERT_TRUE(foreign.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 first, second, wrong;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 1, 640, second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(foreign, program, MemoryKey(), 2, 1, 640, wrong));
    harken::PendingSelectionBatchV1 batch;
    harken::PreparedSelectionPlayV1* invalid[] = {&first, &wrong};
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.MovePreparedPlaysToPending(invalid, 2, 2, &batch));
    harken::PreparedSelectionPlayV1* duplicate[] = {&first, &first};
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.MovePreparedPlaysToPending(duplicate, 2, 2, &batch));
    harken::PreparedSelectionPlayV1* reversed[] = {&second, &first};
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.MovePreparedPlaysToPending(reversed, 2, 2, &batch));
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_TRUE(wrong.Valid());
    EXPECT_FALSE(batch.Valid());
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(6u, table.FreeDeltaCount());
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(second).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, foreign.CancelPreparedPlay(&wrong));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingBatchCancellationRecomputesRemainingHiddenReservation) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 first, second, third;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 1, 640, second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 2, 640, third));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batch));
    EXPECT_FALSE(batch.Valid());
    EXPECT_EQ(1u, table.PreparedPlaySeedProbe(third).selectionOrdinal);
    harken::SelectionDeltaRefV1 remaining;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlayToPending(&third, 2, &remaining));
    EXPECT_EQ(5u, table.Delta(remaining).selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(remaining));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingBatchCommitsDifferentKeysTogether) {
    MemoryProgram global(harken::ContainerModeV1::Sequence), emitter(harken::ContainerModeV1::Sequence);
    global.program.memoryScope = harken::MemoryScopeV1::Global;
    const auto globalKey = MemoryKey(harken::MemoryScopeV1::Global);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, global, globalKey, 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, emitter, MemoryKey(), 2, 1, 640, second));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    EXPECT_EQ(0u, table.Snapshot(globalKey).selectionOrdinal);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    EXPECT_EQ(1u, table.Snapshot(globalKey).selectionOrdinal);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckPendingBatchMixedIdentityFailure(bool differentWorld) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto otherKey = MemoryKey();
    if (differentWorld) otherKey.world = 4;
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, otherKey, 2, 1, differentWorld ? 640u : 960u, second));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_FALSE(batch.Valid());
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(0u, table.RowCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&first));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingBatchRejectsMixedWorldWithoutConsumingTokens) {
    CheckPendingBatchMixedIdentityFailure(true);
}

TEST(CurrentSelectionMemory, PendingBatchRejectsMixedPlacementWithoutConsumingTokens) {
    CheckPendingBatchMixedIdentityFailure(false);
}

static void CheckPendingBatchMixedScopeRetirement(bool retireWorld) {
    MemoryProgram global(harken::ContainerModeV1::Sequence), emitter(harken::ContainerModeV1::Sequence);
    global.program.memoryScope = harken::MemoryScopeV1::Global;
    const auto key = MemoryKey(harken::MemoryScopeV1::Global);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto history = table.AppendPlay(MemoryRequest(global, 1, key));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, history.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(history.delta));
    harken::PreparedSelectionPlayV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, global, key, 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, emitter, MemoryKey(), 2, 1, 640, second));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    const auto view = table.ReadPendingBatch(batch);
    if (retireWorld) table.RetireWorld(key.world);
    else table.RetireEmitter(0, 1);
    EXPECT_FALSE(table.ReadPendingBatch(batch).live);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CommitPendingBatch(&batch, view.revision));
    EXPECT_FALSE(table.Snapshot(MemoryKey()).found);
    EXPECT_EQ(!retireWorld, table.Snapshot(key).found);
    EXPECT_EQ(retireWorld ? 0u : 1u, table.Snapshot(key).selectionOrdinal);
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(8u, table.FreeDeltaCount());
    EXPECT_EQ(8u, table.FreePendingBatchCount());
}

TEST(CurrentSelectionMemory, PendingBatchEmitterRetirementCancelsGlobalMembersWithoutRewindingHistory) {
    CheckPendingBatchMixedScopeRetirement(false);
}

TEST(CurrentSelectionMemory, PendingBatchWorldRetirementReclaimsEveryMember) {
    CheckPendingBatchMixedScopeRetirement(true);
}

static void CheckPendingBatchStaleOwner(bool resetTable) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 first;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 1, 0, 640, first));
    harken::PreparedSelectionPlayV1* token[] = {&first};
    harken::PendingSelectionBatchV1 stale;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(token, 1, 1, &stale));
    const auto old = table.ReadPendingBatchMember(stale, 0);
    if (resetTable) ASSERT_TRUE(table.Reset(MemoryConfig()));
    else table.RetireEmitter(0, 1);
    harken::PreparedSelectionPlayV1 second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, second));
    token[0] = &second;
    harken::PendingSelectionBatchV1 next;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(token, 1, 2, &next));
    EXPECT_EQ(old.ref.slot, table.ReadPendingBatchMember(next, 0).ref.slot);
    EXPECT_FALSE(table.ReadPendingBatch(stale).live);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CommitPendingBatch(&stale, old.revision));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CancelPendingBatch(&stale));
    const auto view = table.ReadPendingBatch(next);
    ASSERT_TRUE(view.live);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&next, view.revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, PendingBatchReusedSlotRejectsRetiredOwner) {
    CheckPendingBatchStaleOwner(false);
}

TEST(CurrentSelectionMemory, PendingBatchNewArenaRejectsOldLifetime) {
    CheckPendingBatchStaleOwner(true);
}

static void RetireSelectionStorage(harken::SelectionMemoryTableV1& table, unsigned kind) {
    if (kind == 0) table.RetireEmitter(0, 1);
    else if (kind == 1) table.RetireWorld(3);
    else ASSERT_TRUE(table.Reset(MemoryConfig()));
}

static void CheckStalePreparedPlay(unsigned kind) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, original));
    harken::PreparedSelectionPlayV1 stale(std::move(original));
    RetireSelectionStorage(table, kind);
    EXPECT_EQ(8u, table.FreeDeltaCount());
    harken::PreparedSelectionPlayV1 fresh;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 2, 0, 640, fresh));
    EXPECT_FALSE(table.PreparedPlaySeedProbe(stale).captured);
    harken::PreparedSelectionPlayV1* oldTokens[] = {&stale};
    harken::PendingSelectionBatchV1 batch;
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok,
        table.MovePreparedPlaysToPending(oldTokens, 1, 2, &batch));
    EXPECT_FALSE(batch.Valid());
    harken::SelectionDeltaRefV1 single;
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlayToPending(&stale, 2, &single));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlay(&stale, 2));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlayTransactional(&stale, 2));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&stale));
    ASSERT_TRUE(table.PreparedPlaySeedProbe(fresh).captured);
    harken::PreparedSelectionPlayV1* freshTokens[] = {&fresh};
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.MovePreparedPlaysToPending(freshTokens, 1, 2, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, RetiredEmitterRejectsOldPreparedPlay) { CheckStalePreparedPlay(0); }
TEST(CurrentSelectionMemory, RetiredWorldRejectsOldPreparedPlay) { CheckStalePreparedPlay(1); }
TEST(CurrentSelectionMemory, NewArenaRejectsOldPreparedPlay) { CheckStalePreparedPlay(2); }

static void CheckStalePreparedReset(unsigned kind) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 oldPlay;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 1, 0, 320, oldPlay));
    harken::PreparedSelectionResetV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &original, 640));
    harken::PreparedSelectionResetV1 stale(std::move(original));
    RetireSelectionStorage(table, kind);
    harken::PreparedSelectionPlayV1 freshPlay;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 1, 0, 320, freshPlay));
    harken::PreparedSelectionResetV1 fresh;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &fresh, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlay(&freshPlay, 1));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&stale, 2));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&stale));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&fresh, 2));
    table.ApplyDue(640);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, RetiredEmitterRejectsOldPreparedReset) { CheckStalePreparedReset(0); }
TEST(CurrentSelectionMemory, RetiredWorldRejectsOldPreparedReset) { CheckStalePreparedReset(1); }
TEST(CurrentSelectionMemory, NewArenaRejectsOldPreparedReset) { CheckStalePreparedReset(2); }

static void CheckStaleSelectionTransaction(unsigned kind) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 stale;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 1, 0, 640, stale));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlayTransactional(&stale, 1));
    RetireSelectionStorage(table, kind);
    EXPECT_EQ(8u, table.FreeDeltaCount());
    harken::PreparedSelectionPlayV1 fresh;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 1, 0, 640, fresh));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlayTransactional(&fresh, 1));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.RollbackCommittedPlay(&stale));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.FinalizeCommittedPlay(&stale));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.FinalizeCommittedPlay(&fresh));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, RetiredEmitterReclaimsOldSelectionTransaction) { CheckStaleSelectionTransaction(0); }
TEST(CurrentSelectionMemory, RetiredWorldReclaimsOldSelectionTransaction) { CheckStaleSelectionTransaction(1); }
TEST(CurrentSelectionMemory, NewArenaRejectsOldSelectionTransaction) { CheckStaleSelectionTransaction(2); }

TEST(CurrentSelectionMemory, PendingBatchRejectsUnsettledTransactionBeforeMovingAnyKey) {
    MemoryProgram global(harken::ContainerModeV1::Sequence), emitter(harken::ContainerModeV1::Sequence);
    global.program.memoryScope = harken::MemoryScopeV1::Global;
    const auto globalKey = MemoryKey(harken::MemoryScopeV1::Global);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 transaction, first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, emitter, MemoryKey(), 1, 0, 320, transaction));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlayTransactional(&transaction, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, global, globalKey, 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, emitter, MemoryKey(), 2, 1, 640, second));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    EXPECT_FALSE(batch.Valid());
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(1u, table.RowCount());
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(second).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.FinalizeCommittedPlay(&transaction));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.RollbackCommittedPlay(&transaction));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(1u, table.Snapshot(globalKey).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckBatchThenHiddenCancellation(bool crossKey) {
    MemoryProgram global(harken::ContainerModeV1::Sequence), emitter(harken::ContainerModeV1::Sequence);
    global.program.memoryScope = harken::MemoryScopeV1::Global;
    auto config = MemoryConfig();
    config.maxGlobalRows = config.maxEmitterRowsPerEmitter = 1;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    harken::PreparedSelectionPlayV1 first, second, hidden;
    const auto firstKey = crossKey ? MemoryKey(harken::MemoryScopeV1::Global) : MemoryKey();
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, crossKey ? global : emitter, firstKey, 2, 0, 640, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, emitter, MemoryKey(), 2, 1, 640, second));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, emitter, MemoryKey(), 2, 2, 640, hidden));
    harken::PreparedSelectionPlayV1* tokens[] = {&first, &second};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 2, 2, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batch));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_FALSE(table.Snapshot(MemoryKey()).found);
    ASSERT_TRUE(table.PreparedPlaySeedProbe(hidden).captured);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&hidden));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(8u, table.FreeDeltaCount());
    EXPECT_FALSE(table.Snapshot(firstKey).found);
    emitter.program.containerStableId = harkentest::ObjectId(101);
    auto nextKey = MemoryKey();
    nextKey.containerStableId = emitter.program.containerStableId;
    const auto next = table.AppendPlay(MemoryRequest(emitter, 3, nextKey));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, next.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(next.delta));
    EXPECT_EQ(1u, table.Snapshot(nextKey).selectionOrdinal);
}

TEST(CurrentSelectionMemory, SameKeyBatchAndHiddenCancellationReleasesRowCapacity) { CheckBatchThenHiddenCancellation(false); }
TEST(CurrentSelectionMemory, CrossKeyBatchAndHiddenCancellationReleasesRowCapacity) { CheckBatchThenHiddenCancellation(true); }

TEST(CurrentSelectionMemory, OldBatchMemberReferenceCannotConsumeNewArenaOrdinaryPlay) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 prepared;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 1, 0, 640, prepared));
    harken::PreparedSelectionPlayV1* tokens[] = {&prepared};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 1, 1, &batch));
    const auto stale = table.ReadPendingBatchMember(batch, 0).ref;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto fresh = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, fresh.status);
    ASSERT_EQ(stale.slot, fresh.delta.slot);
    ASSERT_EQ(stale.generation, fresh.delta.generation);
    EXPECT_FALSE(table.Delta(stale).live);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CommitHead(stale));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.FailHead(stale));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CancelDelta(stale));
    ASSERT_TRUE(table.Delta(fresh.delta).live);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(fresh.delta));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, NewArenaRejectsOldIdempotentPreparedReset) {
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionResetV1 stale;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 1, 0, &stale));
    ASSERT_TRUE(stale.Idempotent());
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&stale, 1));
    EXPECT_NE(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&stale));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(8u, table.FreeDeltaCount());
    harken::PreparedSelectionResetV1 fresh;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 1, 0, &fresh));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&fresh, 1));
}

static void PrepareHiddenScheduledSelectionReset(harken::SelectionMemoryTableV1& table,
    const MemoryProgram& program) {
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 hidden;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 1, 0, 320, hidden));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &reset, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&hidden));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, 2));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(7u, table.FreeDeltaCount());
}

static void CheckOrdinaryFollowerCancellationRetainsScheduledReset(bool failHead) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_NO_FATAL_FAILURE(PrepareHiddenScheduledSelectionReset(table, program));
    harken::PreparedSelectionPlayV1 prepared;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 3, 0, 1280, prepared));
    harken::PreparedSelectionPlayV1* tokens[] = {&prepared};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 1, 3, &batch));
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 1600;
    const auto follower = table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, follower.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        failHead ? table.FailHead(follower.delta) : table.CancelDelta(follower.delta));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_FALSE(table.Snapshot(MemoryKey()).found);
    EXPECT_EQ(7u, table.FreeDeltaCount());
    table.ApplyDue(640);
    ASSERT_TRUE(table.Snapshot(MemoryKey()).found);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(1u, table.RowCount());
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, OrdinaryFollowerCancelRetainsScheduledResetAfterBatchCancellation) {
    CheckOrdinaryFollowerCancellationRetainsScheduledReset(false);
}

TEST(CurrentSelectionMemory, OrdinaryFollowerFailureRetainsScheduledResetAfterBatchCancellation) {
    CheckOrdinaryFollowerCancellationRetainsScheduledReset(true);
}

static void CheckOrdinaryAppendRestoresHiddenRowCount(bool resetAppend) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_NO_FATAL_FAILURE(PrepareHiddenScheduledSelectionReset(table, program));
    harken::PreparedSelectionPlayV1 prepared;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        PrepareMemoryBatchMember(table, program, MemoryKey(), 3, 0, 1280, prepared));
    harken::PreparedSelectionPlayV1* tokens[] = {&prepared};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 1, 3, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batch));
    ASSERT_EQ(0u, table.RowCount());
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 1600;
    const auto appended = resetAppend ? table.AppendReset(MemoryKey(), 4, 0) : table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, appended.status);
    EXPECT_EQ(1u, table.RowCount());
    EXPECT_TRUE(table.Snapshot(MemoryKey()).found);
    if (resetAppend) {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(appended.delta));
        table.ApplyDue(640);
        ASSERT_TRUE(table.Snapshot(MemoryKey()).found);
        EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    } else {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(appended.delta));
        EXPECT_TRUE(table.Snapshot(MemoryKey()).found);
        EXPECT_EQ(1u, table.RowCount());
        EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
        EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
        table.ApplyDue(1600);
        EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    }
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, OrdinaryPlayRestoresHiddenRowCountAfterBatchCancellation) {
    CheckOrdinaryAppendRestoresHiddenRowCount(false);
}

TEST(CurrentSelectionMemory, OrdinaryResetRestoresHiddenRowCountAfterBatchCancellation) {
    CheckOrdinaryAppendRestoresHiddenRowCount(true);
}

static void CheckPendingBatchCancellationWithScheduledReset(bool applies) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto history = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, history.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(history.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &reset, applies ? 640u : 1920u));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, 2));
    harken::PreparedSelectionPlayV1 first;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, PrepareMemoryBatchMember(table, program, MemoryKey(), 3, 0, 1280, first));
    harken::PreparedSelectionPlayV1* tokens[] = {&first};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(tokens, 1, 3, &batch));
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 1600;
    const auto follower = table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, follower.status);
    EXPECT_EQ(applies ? 9u : 2u, table.Delta(follower.delta).selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&batch));
    const auto next = table.Delta(follower.delta);
    ASSERT_TRUE(next.live);
    EXPECT_EQ(applies ? 5u : 9u, next.selectedChildOrdinal);
    EXPECT_EQ(applies ? 1u : 0u, next.resetEpochAfter);
    EXPECT_EQ(applies ? 1u : 2u, next.selectionOrdinalAfter);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(follower.delta));
    table.ApplyDue(1920);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(applies ? 1u : 0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingBatchCancellationKeepsApplicableResetForOrdinaryFollower) {
    CheckPendingBatchCancellationWithScheduledReset(true);
}

TEST(CurrentSelectionMemory, PendingBatchCancellationDoesNotApplyFutureResetToOrdinaryFollower) {
    CheckPendingBatchCancellationWithScheduledReset(false);
}

TEST(CurrentSelectionMemory, PendingFifoRetainsReverseFrameSelectionsAfterMiddleCancellation) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    std::array<harken::SelectionDeltaRefV1, 3> pending{};
    const std::array<std::uint64_t, 3> frames{1280, 640, 320};
    const std::array<std::uint32_t, 3> choices{5, 9, 2};
    for (std::uint32_t index = 0; index < frames.size(); ++index) {
        auto request = MemoryRequest(program, index + 1u);
        request.executeFrame = frames[index];
        const auto play = table.AppendPlay(request);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, play.status);
        pending[index] = play.delta;
        EXPECT_EQ(choices[index], table.Delta(play.delta).selectedChildOrdinal);
    }
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(pending[1]));
    EXPECT_EQ(9u, table.Delta(pending[2]).selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(pending[0]));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(pending[2]));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

static void CheckPendingFifoResetAndScheduledPrefix(bool firstPlayIncludesScheduled) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto history = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, history.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(history.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &reset, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, 2));
    auto request = MemoryRequest(program, 3);
    request.executeFrame = firstPlayIncludesScheduled ? 1280u : 320u;
    const auto first = table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    const auto immediateReset = table.AppendReset(MemoryKey(), 4, 0);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, immediateReset.status);
    request.ticket = request.acceptedPostOrdinal = 5;
    request.executeFrame = 1600;
    const auto last = table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, last.status);
    EXPECT_EQ(2u, table.Delta(last.delta).resetEpochAfter);
    EXPECT_EQ(1u, table.Delta(last.delta).selectionOrdinalAfter);
    EXPECT_EQ(5u, table.Delta(last.delta).selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(first.delta));
    EXPECT_EQ(2u, table.Delta(last.delta).resetEpochAfter);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(immediateReset.delta));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(last.delta));
    table.ApplyDue(1600);
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingFifoResetKeepsAlreadyIncludedScheduledEpoch) {
    CheckPendingFifoResetAndScheduledPrefix(true);
}

TEST(CurrentSelectionMemory, PendingFifoResetAccumulatesNewlyApplicableScheduledEpoch) {
    CheckPendingFifoResetAndScheduledPrefix(false);
}

static void CheckPendingPlaySameFrameScheduledResetBoundary(std::uint64_t ticket, std::uint32_t event) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto history = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, history.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(history.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), ticket, event, &reset, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, ticket));
    auto request = MemoryRequest(program, 3);
    request.eventIndex = 2;
    request.executeFrame = 640;
    const auto play = table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, play.status);
    const bool applies = ticket < request.ticket || (ticket == request.ticket && event < request.eventIndex);
    EXPECT_EQ(applies ? 1u : 0u, table.Delta(play.delta).resetEpochAfter);
    EXPECT_EQ(applies ? 5u : 9u, table.Delta(play.delta).selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(play.delta));
    table.ApplyDue(640);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, PendingPlayIncludesEarlierSameFrameResetEvent) {
    CheckPendingPlaySameFrameScheduledResetBoundary(3, 1);
}

TEST(CurrentSelectionMemory, PendingPlayExcludesEqualSameFrameResetEvent) {
    CheckPendingPlaySameFrameScheduledResetBoundary(3, 2);
}

TEST(CurrentSelectionMemory, PendingPlayExcludesLaterSameFrameResetEvent) {
    CheckPendingPlaySameFrameScheduledResetBoundary(3, 3);
}

TEST(CurrentSelectionMemory, PendingPlayExcludesLaterSameFrameResetTicket) {
    CheckPendingPlaySameFrameScheduledResetBoundary(4, 0);
}

TEST(CurrentSelectionMemory, OldWorldReclamationPreservesEveryUnsettledSelectionOwner) {
    for (unsigned kind = 0; kind < 5; ++kind) {
        SCOPED_TRACE(kind);
        MemoryProgram program;
        harken::SelectionMemoryTableV1 table;
        ASSERT_TRUE(table.Reset(MemoryConfig()));
        const auto first = table.AppendPlay(MemoryRequest(program, 1));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(first.delta));
        harken::PreparedSelectionPlayV1 play;
        harken::PreparedSelectionResetV1 reset;
        harken::SelectionDeltaRefV1 pending{};
        std::uint32_t selected = 0;
        bool reader = kind == 4;
        const auto hasReaders = [](const void* context, harken::WorldGeneration) noexcept {
            return *static_cast<const bool*>(context);
        };
        if (kind <= 2) {
            ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
                table.PreparePlay(MemoryRequest(program, 2), &play, &selected));
            if (kind == 1) ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
                table.CommitPreparedPlayTransactional(&play, 2));
            if (kind == 2) ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
                table.MovePreparedPlayToPending(&play, 2, &pending));
        } else if (kind == 3) {
            ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
                table.PrepareReset(MemoryKey(), 2, 0, &reset, 100));
            ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, 2));
        }
        table.RetireUnusedWorldsBefore(4, hasReaders, &reader);
        ASSERT_TRUE(table.Snapshot(MemoryKey()).found);
        if (kind == 0) ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
        if (kind == 1) ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.FinalizeCommittedPlay(&play));
        if (kind == 2) ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(pending));
        if (kind == 3) table.ApplyDue(100);
        reader = false;
        table.RetireUnusedWorldsBefore(3, hasReaders, &reader);
        ASSERT_TRUE(table.Snapshot(MemoryKey()).found);
        table.RetireUnusedWorldsBefore(4, hasReaders, &reader);
        EXPECT_FALSE(table.Snapshot(MemoryKey()).found);
        EXPECT_EQ(0u, table.RowCount());
        EXPECT_EQ(MemoryConfig().maxPendingDeltasTotal, table.FreeDeltaCount());
    }
}

static harken::SelectionPendingBatchIdentityV1 ProducerIdentity(const harken::SelectionPlayRequestV1& request) {
    harken::SelectionPendingBatchIdentityV1 identity{};
    identity.world = request.key.world;
    identity.worldSemanticFingerprint = request.worldSemanticFingerprint;
    identity.rootStableId = request.rootStableId;
    identity.callerSeed = request.callerSeed;
    identity.acceptedPostOrdinal = request.acceptedPostOrdinal;
    identity.ticket = request.ticket;
    identity.emitterSlot = 0;
    identity.emitterGeneration = 1;
    identity.eventCount = 32;
    return identity;
}

static harken::SelectionMemoryStatusV1 IssueMemoryProducerBatch(harken::SelectionMemoryTableV1& table,
    const harken::SelectionPlayRequestV1& request, harken::PendingSelectionBatchV1& batch) {
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    const auto status = table.PreparePlayForPendingBatch(request, &play, &selected);
    if (status != harken::SelectionMemoryStatusV1::Ok) return status;
    harken::PreparedSelectionPlayV1* members[] = {&play};
    return table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), members, 1, &batch);
}

static void SetSecondMemoryProgram(MemoryProgram& program, harken::SelectionMemoryKeyV1& key) {
    key.containerStableId = program.program.containerStableId = harkentest::ObjectId(101);
    key.programSemanticFingerprint = program.program.programSemanticFingerprint = 201;
}

TEST(CurrentSelectionMemory, ReplacementRequiresExplicitSealIncludingIntentionalEmpty) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto request = MemoryRequest(program, 1);
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &batch));
    ASSERT_TRUE(table.ReadPendingBatch(batch).live);
    EXPECT_EQ(0u, table.ReadPendingBatch(batch).memberCount);
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.BeginPendingBatchReplacement(batch, table.ReadPendingBatch(batch).revision, &replacement));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.InstallReplacement(&replacement, &batch));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.SealReplacement(&replacement, 1, 0));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&replacement, 0, 0));
    std::uint32_t selected = 0;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.PrepareReplacementPlay(&replacement, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&replacement, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, table.ReadPendingBatch(batch).revision));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(8u, table.FreeDeltaCount());
    EXPECT_EQ(8u, table.FreePendingBatchCount());
}

TEST(CurrentSelectionMemory, ReplacementCrossKeyPrecedesStillWaitingLaterTicket) {
    MemoryProgram first(harken::ContainerModeV1::Sequence), second(harken::ContainerModeV1::Sequence);
    auto key = MemoryKey();
    SetSecondMemoryProgram(second, key);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 original, follower;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(first, 1), original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(second, 2, key), follower));
    const auto followerBefore = table.ReadPendingBatch(follower);
    const auto oldMember = table.ReadPendingBatchMember(original, 0);
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.BeginPendingBatchReplacement(original, table.ReadPendingBatch(original).revision, &replacement));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&replacement, MemoryRequest(second, 1, key), &selected));
    EXPECT_EQ(5u, selected);
    EXPECT_EQ(5u, table.ReadPendingBatchMember(follower, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(oldMember.ref.slot, table.ReadPendingBatchMember(original, 0).ref.slot);
    EXPECT_EQ(2u, table.PendingCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&replacement, 1, 0));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&replacement, &original));
    EXPECT_FALSE(table.Delta(oldMember.ref).live);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(follower, 0).delta.selectedChildOrdinal);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CanCommitPendingBatch(follower, followerBefore.revision));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.CommitPendingBatch(&follower, table.ReadPendingBatch(follower).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&follower, table.ReadPendingBatch(follower).revision));
    EXPECT_EQ(2u, table.Snapshot(key).selectionOrdinal);
    EXPECT_FALSE(table.Snapshot(MemoryKey()).found);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementNewKeyUsesActualPlayedHistoryWithoutRewind) {
    MemoryProgram first(harken::ContainerModeV1::Sequence), second(harken::ContainerModeV1::Sequence);
    auto key = MemoryKey();
    SetSecondMemoryProgram(second, key);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 original, played;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(first, 1), original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(second, 2, key), played));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&played, table.ReadPendingBatch(played).revision));
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.BeginPendingBatchReplacement(original, table.ReadPendingBatch(original).revision, &replacement));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&replacement, MemoryRequest(second, 1, key), &selected));
    EXPECT_EQ(9u, selected);
    EXPECT_EQ(1u, table.Snapshot(key).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&replacement, 1, 0));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&replacement, &original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    EXPECT_EQ(2u, table.Snapshot(key).selectionOrdinal);
}

TEST(CurrentSelectionMemory, ReplacementSameKeySkipsOwnOldMemberAndAdvancesOnlyAtCommit) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 original, follower;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 2), follower));
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.BeginPendingBatchReplacement(original, table.ReadPendingBatch(original).revision, &replacement));
    auto request = MemoryRequest(program, 1);
    request.executeFrame = 900;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&replacement, request, &selected));
    EXPECT_EQ(5u, selected);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&replacement, 1, 900));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&replacement, &original));
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(1u, table.ReadPendingBatchMember(original, 0).delta.ticket);
    EXPECT_EQ(0u, table.ReadPendingBatchMember(original, 0).delta.eventIndex);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(follower, 0).delta.selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&follower));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, ReplacementNonemptyEmptyNonemptyKeepsProducerAndRebasesFollower) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 original, follower;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 2), follower));
    harken::PendingSelectionReplacementV1 empty;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.BeginPendingBatchReplacement(original, table.ReadPendingBatch(original).revision, &empty));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&empty, 0, 100));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&empty, &original));
    EXPECT_TRUE(table.ReadPendingBatch(original).live);
    EXPECT_EQ(0u, table.ReadPendingBatch(original).memberCount);
    EXPECT_EQ(5u, table.ReadPendingBatchMember(follower, 0).delta.selectedChildOrdinal);
    harken::PendingSelectionReplacementV1 audible;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.BeginPendingBatchReplacement(original, table.ReadPendingBatch(original).revision, &audible));
    auto request = MemoryRequest(program, 1);
    request.executeFrame = 200;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&audible, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&audible, 1, 200));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&audible, &original));
    EXPECT_EQ(5u, selected);
    EXPECT_EQ(9u, table.ReadPendingBatchMember(follower, 0).delta.selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&follower, table.ReadPendingBatch(follower).revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, ReplacementMutationRejectsStaleCandidateWithoutTouchingOriginal) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), original));
    const auto before = table.ReadPendingBatchMember(original, 0);
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.BeginPendingBatchReplacement(original, table.ReadPendingBatch(original).revision, &replacement));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&replacement, MemoryRequest(program, 1), &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&replacement, 1, 0));
    const auto later = table.AppendPlay(MemoryRequest(program, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, later.status);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.InstallReplacement(&replacement, &original));
    EXPECT_EQ(before.ref.slot, table.ReadPendingBatchMember(original, 0).ref.slot);
    EXPECT_EQ(before.ref.generation, table.ReadPendingBatchMember(original, 0).ref.generation);
    EXPECT_EQ(before.revision, table.ReadPendingBatch(original).revision);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&replacement));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(later.delta));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementNoChangeApplyDueAndOtherCandidateCancelRemainCurrent) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(MemoryRequest(program, 1)), nullptr, 0, &first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(MemoryRequest(program, 2)), nullptr, 0, &second));
    harken::PendingSelectionReplacementV1 a, b;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(first, 1, &a));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(second, 1, &b));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&a, MemoryRequest(program, 1), &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&b, MemoryRequest(program, 2), &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&a, 1, 0));
    table.ApplyDue(1000);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&b));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CanInstallReplacement(a));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&a, &first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&second));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementCapacityFailurePreservesOriginalAndPreparedPrefix) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    auto config = MemoryConfig();
    config.maxPendingDeltasTotal = 2;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), original));
    const auto old = table.ReadPendingBatchMember(original, 0);
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, old.revision, &replacement));
    auto request = MemoryRequest(program, 1);
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&replacement, request, &selected));
    request.eventIndex = 1;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Capacity, table.PrepareReplacementPlay(&replacement, request, &selected));
    EXPECT_EQ(old.ref.slot, table.ReadPendingBatchMember(original, 0).ref.slot);
    EXPECT_EQ(old.revision, table.ReadPendingBatch(original).revision);
    EXPECT_EQ(0u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&replacement));
    EXPECT_EQ(1u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, old.revision));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(2u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementLegacyBatchCannotAcquireStrictProducerIdentity) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(MemoryRequest(program, 1), &play, &selected));
    harken::PreparedSelectionPlayV1* members[] = {&play};
    harken::PendingSelectionBatchV1 legacy;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(members, 1, 1, &legacy));
    harken::PendingSelectionReplacementV1 replacement;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.BeginPendingBatchReplacement(legacy, 1, &replacement));
    EXPECT_FALSE(replacement.Valid());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&legacy, 1));
}

TEST(CurrentSelectionMemory, ReplacementProducerRejectsIdentityDomainAndLegacyPreparedWithoutTransfer) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto request = MemoryRequest(program, 1);
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(request, &play, &selected));
    harken::PreparedSelectionPlayV1* members[] = {&play};
    harken::PendingSelectionBatchV1 batch;
    auto identity = ProducerIdentity(request);
    ++identity.callerSeed;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.IssueProducerPendingSelectionBatch(identity, members, 1, &batch));
    EXPECT_TRUE(play.Valid());
    identity = ProducerIdentity(request);
    identity.eventBegin = UINT32_MAX;
    identity.eventCount = 2;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.IssueProducerPendingSelectionBatch(identity, nullptr, 0, &batch));
    identity = ProducerIdentity(request);
    identity.eventCount = 0;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.IssueProducerPendingSelectionBatch(identity, nullptr, 0, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &play, &selected));
    identity = ProducerIdentity(request);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.IssueProducerPendingSelectionBatch(identity, members, 1, &batch));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckReplacementRetirement(unsigned mode) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    program.program.memoryScope = harken::MemoryScopeV1::Global;
    const auto key = MemoryKey(harken::MemoryScopeV1::Global);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto request = MemoryRequest(program, 1, key);
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &original));
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &replacement));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&replacement, request, &selected));
    if (mode == 0) table.RetireEmitter(0, 1);
    else if (mode == 1) table.RetireWorld(key.world);
    else ASSERT_TRUE(table.Reset(MemoryConfig()));
    EXPECT_FALSE(table.ReadPendingBatch(original).live);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CancelReplacement(&replacement));
    EXPECT_EQ(8u, table.FreeDeltaCount());
    EXPECT_EQ(8u, table.FreePendingBatchCount());
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(0u, table.PendingCount());
    harken::PendingSelectionBatchV1 next;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &next));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CancelReplacement(&replacement));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&next));
}

TEST(CurrentSelectionMemory, ReplacementEmptyProducerEmitterRetirementReclaimsGlobalCandidate) { CheckReplacementRetirement(0); }
TEST(CurrentSelectionMemory, ReplacementEmptyProducerWorldRetirementReclaimsCandidate) { CheckReplacementRetirement(1); }
TEST(CurrentSelectionMemory, ReplacementNewArenaRejectsOldCandidate) { CheckReplacementRetirement(2); }

TEST(CurrentSelectionMemory, ReplacementSuccessfulOriginalCommitAutomaticallyCancelsPrivateCandidate) {
    MemoryProgram first(harken::ContainerModeV1::Sequence), second(harken::ContainerModeV1::Sequence);
    auto key = MemoryKey();
    SetSecondMemoryProgram(second, key);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(first, 1), original));
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &replacement));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&replacement, MemoryRequest(second, 1, key), &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, 1));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_FALSE(table.Snapshot(key).found);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CancelReplacement(&replacement));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementRejectsScheduledResetWithSameTicketAndEvent) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto history = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, history.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(history.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 3, 0, &reset, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, 3));
    auto request = MemoryRequest(program, 3);
    request.executeFrame = 640;
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &original));
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    const auto freeBefore = table.FreeDeltaCount();
    std::uint32_t selected = 0;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.PrepareReplacementPlay(&candidate, request, &selected));
    EXPECT_EQ(freeBefore, table.FreeDeltaCount());
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.SealReplacement(&candidate, 1, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&original));
    table.ApplyDue(640);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckReplacementDoesNotOvertakeNativeHidden(bool sameTicket) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto request = MemoryRequest(program, 3);
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &original));
    auto hiddenRequest = MemoryRequest(program, sameTicket ? 3 : 2);
    hiddenRequest.eventIndex = sameTicket ? 4 : 0;
    harken::PreparedSelectionPlayV1 hidden;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(hiddenRequest, &hidden, &selected));
    const auto seedBefore = table.PreparedPlaySeedProbe(hidden);
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    if (sameTicket) {
        request.eventIndex = 2;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
        request.eventIndex = 6;
    }
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.PrepareReplacementPlay(&candidate, request, &selected));
    EXPECT_EQ(seedBefore.selectionOrdinal, table.PreparedPlaySeedProbe(hidden).selectionOrdinal);
    EXPECT_EQ(seedBefore.acceptedPostOrdinal, table.PreparedPlaySeedProbe(hidden).acceptedPostOrdinal);
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(1u, table.ReadPendingBatch(original).revision);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&candidate));
    harken::PreparedSelectionPlayV1* members[] = {&hidden};
    harken::PendingSelectionBatchV1 earlier;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(hiddenRequest), members, 1, &earlier));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&earlier, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&original));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementCannotOvertakeEarlierTicketNativeHidden) { CheckReplacementDoesNotOvertakeNativeHidden(false); }
TEST(CurrentSelectionMemory, ReplacementCannotCrossEarlierEventNativeHiddenWithinSameTicket) { CheckReplacementDoesNotOvertakeNativeHidden(true); }

TEST(CurrentSelectionMemory, ReplacementRecomputesLaterNativeHiddenAndAllowsItsAuthenticTransfer) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto request = MemoryRequest(program, 3);
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &original));
    const auto laterRequest = MemoryRequest(program, 4);
    harken::PreparedSelectionPlayV1 hidden;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(laterRequest, &hidden, &selected));
    EXPECT_EQ(5u, selected);
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 1, 0));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&candidate, &original));
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(hidden).selectionOrdinal);
    harken::PreparedSelectionPlayV1* members[] = {&hidden};
    harken::PendingSelectionBatchV1 later;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(laterRequest), members, 1, &later));
    EXPECT_EQ(9u, table.ReadPendingBatchMember(later, 0).delta.selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&later, table.ReadPendingBatch(later).revision));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, ReplacementMultipleKeysAndEventsInstallTogetherAndKeepUnrelatedRevision) {
    MemoryProgram first(harken::ContainerModeV1::Sequence), second(harken::ContainerModeV1::Sequence);
    second.program.memoryScope = harken::MemoryScopeV1::Global;
    auto key = MemoryKey(harken::MemoryScopeV1::Global);
    SetSecondMemoryProgram(second, key);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 original, follower, unrelated;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(first, 1), original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(second, 3, key), follower));
    auto unrelatedRequest = MemoryRequest(first, 4);
    unrelatedRequest.key.emitterSlot = 1;
    auto unrelatedIdentity = ProducerIdentity(unrelatedRequest);
    unrelatedIdentity.emitterSlot = 1;
    harken::PreparedSelectionPlayV1 unrelatedPlay;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(unrelatedRequest, &unrelatedPlay, &selected));
    harken::PreparedSelectionPlayV1* unrelatedMembers[] = {&unrelatedPlay};
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(unrelatedIdentity, unrelatedMembers, 1, &unrelated));
    const auto unrelatedBefore = table.ReadPendingBatch(unrelated);
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    auto request = MemoryRequest(second, 1, key);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    EXPECT_EQ(5u, selected);
    request = MemoryRequest(first, 1);
    request.eventIndex = 1;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    EXPECT_EQ(5u, selected);
    request = MemoryRequest(second, 1, key);
    request.eventIndex = 2;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    EXPECT_EQ(9u, selected);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 3, 0));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&candidate, &original));
    EXPECT_EQ(unrelatedBefore.revision, table.ReadPendingBatch(unrelated).revision);
    EXPECT_EQ(2u, table.ReadPendingBatchMember(original, 2).delta.eventIndex);
    EXPECT_EQ(2u, table.ReadPendingBatchMember(follower, 0).delta.selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&follower, table.ReadPendingBatch(follower).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&unrelated));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(3u, table.Snapshot(key).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckReplacementScheduledReset(bool future) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto history = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, history.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(history.delta));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &reset, future ? 900 : 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&reset, 2));
    auto request = MemoryRequest(program, 3);
    request.executeFrame = 640;
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &original));
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    const auto before = table.ReadReplacementPlay(candidate, 0);
    EXPECT_EQ(future ? 9u : 5u, selected);
    EXPECT_EQ(future ? 0u : 1u, before.seedProbe.resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 1, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&candidate, &original));
    const auto after = table.ReadPendingBatchMember(original, 0);
    EXPECT_EQ(selected, after.delta.selectedChildOrdinal);
    EXPECT_EQ(before.seedProbe.resetEpoch, after.seedProbe.resetEpoch);
    EXPECT_EQ(before.seedProbe.selectionOrdinal, after.seedProbe.selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, after.revision));
    table.ApplyDue(900);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(future ? 0u : 1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementIncludesApplicableScheduledReset) { CheckReplacementScheduledReset(false); }
TEST(CurrentSelectionMemory, ReplacementExcludesFutureScheduledReset) { CheckReplacementScheduledReset(true); }

static void CheckReplacementLegacyPreparationIsolation(bool transaction) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(MemoryRequest(program, 3)), nullptr, 0, &original));
    harken::PreparedSelectionPlayV1 play;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(MemoryRequest(program, 2), &play, &selected));
    if (transaction) ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedPlayTransactional(&play, 2));
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    const auto freeBefore = table.FreeDeltaCount();
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead,
        table.PrepareReplacementPlay(&candidate, MemoryRequest(program, 3), &selected));
    EXPECT_EQ(freeBefore, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&candidate));
    if (transaction) ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.FinalizeCommittedPlay(&play));
    else ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&play));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&original));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementRejectsLegacyHiddenWithoutConsumingIt) { CheckReplacementLegacyPreparationIsolation(false); }
TEST(CurrentSelectionMemory, ReplacementRejectsUnsettledTransactionWithoutRewindingIt) { CheckReplacementLegacyPreparationIsolation(true); }

static void CheckReplacementOwnedRandomRequest(harken::RandomDisciplineV1 discipline) {
    for (std::uint64_t caller = 1; caller <= 8; ++caller) {
        SCOPED_TRACE(caller);
        MemoryProgram first(harken::ContainerModeV1::Sequence), second(harken::ContainerModeV1::Random, discipline);
        auto key = MemoryKey();
        SetSecondMemoryProgram(second, key);
        harken::SelectionMemoryTableV1 table, reference;
        ASSERT_TRUE(table.Reset(MemoryConfig()));
        ASSERT_TRUE(reference.Reset(MemoryConfig()));
        auto rootRequest = MemoryRequest(first, 1);
        rootRequest.callerSeed = caller;
        harken::PendingSelectionBatchV1 original, follower;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, rootRequest, original));
        const auto historyRequest = MemoryRequest(second, 2, key);
        const auto history = table.AppendPlay(historyRequest), expectedHistory = reference.AppendPlay(historyRequest);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, history.status);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, expectedHistory.status);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(history.delta));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, reference.CommitHead(expectedHistory.delta));
        const auto followerRequest = MemoryRequest(second, 3, key);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, followerRequest, follower));
        harken::SelectionMemoryTableV1::BackingPreparation backing;
        ASSERT_TRUE(table.PrepareBacking({2, 2, 1, {second.program.avoidLastN,
            discipline == harken::RandomDisciplineV1::Rotation ? second.program.childCount : 0u,
            2u, second.program.childCount}}, &backing));
        const auto preparedBytes = table.StorageBytes();
        table.SetCommitPageLimitForTests(0);
        auto use = backing.Use();
        harken::PendingSelectionReplacementV1 candidate;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
        auto request = MemoryRequest(second, 1, key);
        request.callerSeed = caller;
        std::array<harken::SelectionPathNodeV1, 2> path{};
        path[0].kind = harken::PlayableKindV1::Container;
        path[0].objectId = rootRequest.rootStableId;
        path[0].childOrdinal = 7;
        path[1].kind = harken::PlayableKindV1::Container;
        path[1].objectId = key.containerStableId;
        path[1].childOrdinal = 11;
        request.path = path.data();
        request.pathCount = static_cast<std::uint32_t>(path.size());
        request.occurrenceOrdinal = 11;
        request.loopIteration = 2;
        const auto expected = reference.AppendPlay(request);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, expected.status);
        const auto expectedFollower = reference.AppendPlay(followerRequest);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, expectedFollower.status);
        std::uint32_t selected = 0;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
        EXPECT_EQ(reference.Delta(expected.delta).selectedChildOrdinal, selected);
        const auto prepared = table.ReadReplacementPlay(candidate, 0);
        path[0].objectId = {};
        path[0].childOrdinal = 99;
        path[1].childOrdinal = 100;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 1, 0));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&candidate, &original));
        const auto installed = table.ReadPendingBatchMember(original, 0);
        EXPECT_EQ(selected, installed.delta.selectedChildOrdinal);
        EXPECT_EQ(caller, installed.seedProbe.derivedCallerSeed);
        EXPECT_EQ(1u, installed.seedProbe.acceptedPostOrdinal);
        EXPECT_EQ(prepared.seedProbe.selectionOrdinal, installed.seedProbe.selectionOrdinal);
        EXPECT_EQ(reference.Delta(expectedFollower.delta).selectedChildOrdinal,
            table.ReadPendingBatchMember(follower, 0).delta.selectedChildOrdinal);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, installed.revision));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&follower, table.ReadPendingBatch(follower).revision));
        EXPECT_EQ(3u, table.Snapshot(key).selectionOrdinal);
        EXPECT_EQ(8u, table.FreeDeltaCount());
        EXPECT_EQ(preparedBytes, table.StorageBytes());
    }
}

TEST(CurrentSelectionMemory, ReplacementDrawKeepsOwnedPathAndAuthenticSeedsThroughInstall) {
    CheckReplacementOwnedRandomRequest(harken::RandomDisciplineV1::Draw);
}
TEST(CurrentSelectionMemory, ReplacementRotationKeepsOwnedPathAndAuthenticSeedsThroughInstall) {
    CheckReplacementOwnedRandomRequest(harken::RandomDisciplineV1::Rotation);
}

TEST(CurrentSelectionMemory, ReplacementFailedOriginalCommitKeepsCandidateAndHiddenCapacity) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 earlier, original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), earlier));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 2), original));
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, MemoryRequest(program, 2), &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 1, 0));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.CommitPendingBatch(&original, 1));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CanInstallReplacement(candidate));
    const auto next = table.AppendPlay(MemoryRequest(program, 3));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, next.status);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Capacity, table.AppendPlay(MemoryRequest(program, 4)).status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&earlier));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(next.delta));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static harken::SelectionMemoryStatusV1 IssueLegacyMemoryPair(harken::SelectionMemoryTableV1& table,
    const harken::SelectionPlayRequestV1& first, const harken::SelectionPlayRequestV1& second,
    harken::PendingSelectionBatchV1& batch) {
    harken::PreparedSelectionPlayV1 prepared[2];
    std::uint32_t selected = 0;
    auto status = table.PreparePlayForPendingBatch(first, &prepared[0], &selected);
    if (status != harken::SelectionMemoryStatusV1::Ok) return status;
    status = table.PreparePlayForPendingBatch(second, &prepared[1], &selected);
    if (status != harken::SelectionMemoryStatusV1::Ok) return status;
    harken::PreparedSelectionPlayV1* members[] = {&prepared[0], &prepared[1]};
    return table.MovePreparedPlaysToPending(members, 2, first.ticket, &batch);
}

TEST(CurrentSelectionMemory, ReplacementRejectsOppositeBatchOrderAcrossKeysBeforeSplice) {
    MemoryProgram first(harken::ContainerModeV1::Sequence), second(harken::ContainerModeV1::Sequence);
    auto secondKey = MemoryKey();
    SetSecondMemoryProgram(second, secondKey);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto earlier = MemoryRequest(first, 1);
    auto later = MemoryRequest(second, 1, secondKey);
    later.eventIndex = 128;
    later.rootStableId = harkentest::ObjectId(302);
    harken::PendingSelectionBatchV1 external, original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueLegacyMemoryPair(table, earlier, later, external));
    const auto oldFirst = table.ReadPendingBatchMember(external, 0);
    const auto oldSecond = table.ReadPendingBatchMember(external, 1);
    auto request = MemoryRequest(first, 1);
    request.eventIndex = 64;
    request.rootStableId = harkentest::ObjectId(303);
    auto identity = ProducerIdentity(request);
    identity.eventBegin = 64;
    identity.eventCount = 64;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(identity, nullptr, 0, &original));
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    request.key = secondKey;
    request.program = &second.program;
    request.eventIndex = 65;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 2, 0));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.CanInstallReplacement(candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.InstallReplacement(&candidate, &original));
    EXPECT_TRUE(candidate.Valid());
    EXPECT_EQ(0u, table.ReadPendingBatch(original).memberCount);
    EXPECT_EQ(oldFirst.ref.slot, table.ReadPendingBatchMember(external, 0).ref.slot);
    EXPECT_EQ(oldSecond.ref.slot, table.ReadPendingBatchMember(external, 1).ref.slot);
    EXPECT_EQ(oldFirst.revision, table.ReadPendingBatch(external).revision);
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&external, oldFirst.revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, 1));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementRejectsTransitiveThreeBatchCycleAcrossKeys) {
    MemoryProgram first(harken::ContainerModeV1::Sequence), second(harken::ContainerModeV1::Sequence),
        third(harken::ContainerModeV1::Sequence);
    auto secondKey = MemoryKey(), thirdKey = MemoryKey();
    SetSecondMemoryProgram(second, secondKey);
    thirdKey.containerStableId = third.program.containerStableId = harkentest::ObjectId(102);
    thirdKey.programSemanticFingerprint = third.program.programSemanticFingerprint = 202;
    auto config = MemoryConfig();
    config.maxEmitterRowsPerEmitter = 3;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    auto xFirst = MemoryRequest(first, 1), xSecond = MemoryRequest(second, 1, secondKey);
    xFirst.eventIndex = 256;
    xFirst.rootStableId = harkentest::ObjectId(304);
    harken::PendingSelectionBatchV1 x, y, original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueLegacyMemoryPair(table, xFirst, xSecond, x));
    auto ySecond = MemoryRequest(second, 1, secondKey), yThird = MemoryRequest(third, 1, thirdKey);
    ySecond.eventIndex = 128;
    ySecond.rootStableId = harkentest::ObjectId(305);
    yThird.eventIndex = 32;
    yThird.rootStableId = harkentest::ObjectId(306);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueLegacyMemoryPair(table, ySecond, yThird, y));
    auto request = MemoryRequest(first, 1);
    request.eventIndex = 64;
    request.rootStableId = harkentest::ObjectId(307);
    auto identity = ProducerIdentity(request);
    identity.eventBegin = 64;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(identity, nullptr, 0, &original));
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    request.key = thirdKey;
    request.program = &third.program;
    request.eventIndex = 65;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 2, 0));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.CanInstallReplacement(candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.InstallReplacement(&candidate, &original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&x, table.ReadPendingBatch(x).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&y, table.ReadPendingBatch(y).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, 1));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckReplacementOrdinaryGap(bool reset) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    if (reset) {
        const auto baseline = table.AppendPlay(MemoryRequest(program, 1));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, baseline.status);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(baseline.delta));
    }
    auto request = MemoryRequest(program, 1);
    request.eventIndex = 64;
    auto identity = ProducerIdentity(request);
    identity.eventBegin = 64;
    identity.eventCount = 64;
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(identity, nullptr, 0, &original));
    auto middle = request;
    middle.eventIndex = 65;
    const auto ordinary = reset ? table.AppendReset(MemoryKey(), 1, 65) : table.AppendPlay(middle);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, ordinary.status);
    ASSERT_TRUE(ordinary.delta);
    ASSERT_TRUE(table.Delta(ordinary.delta).live);
    ASSERT_EQ(1u, table.PendingCount());
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    request.eventIndex = 66;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 2, 0));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.CanInstallReplacement(candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.InstallReplacement(&candidate, &original));
    EXPECT_TRUE(table.Delta(ordinary.delta).live);
    EXPECT_EQ(1u, table.PendingCount());
    EXPECT_EQ(5u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelReplacement(&candidate));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(ordinary.delta));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, 1));
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementRejectsOrdinaryPlayBetweenItsSameKeyMembers) { CheckReplacementOrdinaryGap(false); }
TEST(CurrentSelectionMemory, ReplacementRejectsOrdinaryResetBetweenItsSameKeyMembers) { CheckReplacementOrdinaryGap(true); }

TEST(CurrentSelectionMemory, ReplacementAllowsOrdinaryPrefixAndSuffixAroundContiguousMembers) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    auto config = MemoryConfig();
    config.maxPendingEmitterPerRow = 6;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    auto request = MemoryRequest(program, 1);
    request.eventIndex = 64;
    auto identity = ProducerIdentity(request);
    identity.eventBegin = 64;
    harken::PendingSelectionBatchV1 original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(identity, nullptr, 0, &original));
    const auto before = table.AppendPlay(MemoryRequest(program, 1));
    auto later = request;
    later.eventIndex = 128;
    const auto after = table.AppendPlay(later);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, before.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, after.status);
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    ++request.eventIndex;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 2, 0));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&candidate, &original));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead,
        table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(before.delta));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(after.delta));
    EXPECT_EQ(4u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, ReplacementAllowsOrdinaryBarrierBetweenDifferentBatches) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    auto config = MemoryConfig();
    config.maxPendingEmitterPerRow = 6;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    harken::PendingSelectionBatchV1 before, after, original;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), before));
    const auto barrier = table.AppendPlay(MemoryRequest(program, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, barrier.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 4), after));
    auto request = MemoryRequest(program, 3);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &original));
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(original, 1, &candidate));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 1, 0));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&candidate, &original));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&before, table.ReadPendingBatch(before).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(barrier.delta));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&original, table.ReadPendingBatch(original).revision));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&after, table.ReadPendingBatch(after).revision));
    EXPECT_EQ(4u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

static harken::PendingSelectionBatchCommitV1 MemoryBatchCommit(harken::SelectionMemoryTableV1& table,
    harken::PendingSelectionBatchV1& batch) {
    return {&batch, table.ReadPendingBatch(batch).revision};
}

static void CheckMemoryCohortCommit(std::uint32_t count,
    harken::MemoryScopeV1 scope = harken::MemoryScopeV1::Emitter) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    program.program.memoryScope = scope;
    const auto key = MemoryKey(scope);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 batches[3];
    harken::PendingSelectionBatchCommitV1 inputs[3];
    for (std::uint32_t index = 0; index < count; ++index) {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            IssueMemoryProducerBatch(table, MemoryRequest(program, index + 1u, key), batches[index]));
        inputs[index] = MemoryBatchCommit(table, batches[index]);
    }
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead,
        table.CanCommitPendingBatch(batches[1], inputs[1].revision));
    const auto freeBefore = table.FreeDeltaCount();
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CanCommitPendingBatches(inputs, count));
    EXPECT_EQ(0u, table.Snapshot(key).selectionOrdinal);
    EXPECT_EQ(count, table.PendingCount());
    EXPECT_EQ(freeBefore, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(inputs, count));
    EXPECT_EQ(count, table.Snapshot(key).selectionOrdinal);
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(8u, table.FreeDeltaCount());
    EXPECT_EQ(8u, table.FreePendingBatchCount());
    for (std::uint32_t index = 0; index < count; ++index) EXPECT_FALSE(batches[index].Valid());
}

TEST(CurrentSelectionMemory, CohortCommitTwoSharedKeyProducersAdvancesHistoryTogether) { CheckMemoryCohortCommit(2); }
TEST(CurrentSelectionMemory, CohortCommitThreeSharedKeyProducersAdvancesHistoryTogether) { CheckMemoryCohortCommit(3); }
TEST(CurrentSelectionMemory, CohortCommitThreeGlobalProducersAdvancesHistoryTogether) {
    CheckMemoryCohortCommit(3, harken::MemoryScopeV1::Global);
}

TEST(CurrentSelectionMemory, CohortCommitAcrossKeysConsumesEveryOriginalMember) {
    MemoryProgram first(harken::ContainerModeV1::Sequence), second(harken::ContainerModeV1::Sequence);
    auto secondKey = MemoryKey();
    SetSecondMemoryProgram(second, secondKey);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 batches[3];
    harken::PendingSelectionBatchCommitV1 inputs[3];
    for (std::uint32_t index = 0; index < 3; ++index) {
        auto request = MemoryRequest(first, index + 1u);
        auto other = MemoryRequest(second, index + 1u, secondKey);
        other.eventIndex = 1;
        harken::PreparedSelectionPlayV1 left, right;
        std::uint32_t selected = 0;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(request, &left, &selected));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(other, &right, &selected));
        harken::PreparedSelectionPlayV1* members[] = {&left, &right};
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), members, 2, &batches[index]));
        inputs[index] = MemoryBatchCommit(table, batches[index]);
    }
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(inputs, 3));
    EXPECT_EQ(3u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(3u, table.Snapshot(secondKey).selectionOrdinal);
    EXPECT_EQ(0u, table.PendingCount());
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

static void CheckCohortInvalidInput(std::uint32_t mode) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table, foreign;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    ASSERT_TRUE(foreign.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 first, second, foreignBatch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 2), second));
    harken::PendingSelectionBatchCommitV1 inputs[] = {MemoryBatchCommit(table, first), MemoryBatchCommit(table, second)};
    auto expected = harken::SelectionMemoryStatusV1::InvalidInput;
    if (mode == 0) ++inputs[1].revision;
    if (mode == 1) inputs[1] = inputs[0];
    if (mode == 2) inputs[1].batch = nullptr;
    if (mode == 3) { std::swap(inputs[0], inputs[1]); expected = harken::SelectionMemoryStatusV1::NotHead; }
    if (mode == 4) {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
            IssueMemoryProducerBatch(foreign, MemoryRequest(program, 1), foreignBatch));
        inputs[1] = MemoryBatchCommit(foreign, foreignBatch);
        expected = harken::SelectionMemoryStatusV1::NotFound;
    }
    const auto firstView = table.ReadPendingBatchMember(first, 0);
    const auto secondView = table.ReadPendingBatchMember(second, 0);
    const auto freeBefore = table.FreeDeltaCount();
    EXPECT_EQ(expected, table.CanCommitPendingBatches(inputs, 2));
    EXPECT_EQ(expected, table.CommitPendingBatches(inputs, 2));
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(2u, table.PendingCount());
    EXPECT_EQ(freeBefore, table.FreeDeltaCount());
    const auto firstAfter = table.ReadPendingBatchMember(first, 0).ref;
    const auto secondAfter = table.ReadPendingBatchMember(second, 0).ref;
    EXPECT_EQ(firstView.ref.slot, firstAfter.slot);
    EXPECT_EQ(firstView.ref.generation, firstAfter.generation);
    EXPECT_EQ(firstView.ref.lifetime, firstAfter.lifetime);
    EXPECT_EQ(secondView.ref.slot, secondAfter.slot);
    EXPECT_EQ(secondView.ref.generation, secondAfter.generation);
    EXPECT_EQ(secondView.ref.lifetime, secondAfter.lifetime);
    EXPECT_EQ(secondView.revision, table.ReadPendingBatchMember(second, 0).revision);
    inputs[0] = MemoryBatchCommit(table, first);
    inputs[1] = MemoryBatchCommit(table, second);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(inputs, 2));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
}

TEST(CurrentSelectionMemory, CohortCommitStaleSecondRevisionPreservesEntireCohort) { CheckCohortInvalidInput(0); }
TEST(CurrentSelectionMemory, CohortCommitDuplicateInputPreservesEntireCohort) { CheckCohortInvalidInput(1); }
TEST(CurrentSelectionMemory, CohortCommitNullSecondTokenPreservesEntireCohort) { CheckCohortInvalidInput(2); }
TEST(CurrentSelectionMemory, CohortCommitReversedPublicationOrderPreservesEntireCohort) { CheckCohortInvalidInput(3); }
TEST(CurrentSelectionMemory, CohortCommitForeignArenaPreservesEntireCohort) { CheckCohortInvalidInput(4); }

static void CheckCohortOutsideBlocker(bool ordinary) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 first, outside, last;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), first));
    harken::SelectionDeltaRefV1 blocker;
    if (ordinary) {
        const auto append = table.AppendPlay(MemoryRequest(program, 2));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, append.status);
        blocker = append.delta;
    } else {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 2), outside));
    }
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 3), last));
    harken::PendingSelectionBatchCommitV1 inputs[] = {MemoryBatchCommit(table, first), MemoryBatchCommit(table, last)};
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.CommitPendingBatches(inputs, 2));
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(3u, table.PendingCount());
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(last.Valid());
    EXPECT_EQ(5u, table.FreeDeltaCount());
    if (ordinary) {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelDelta(blocker));
    } else {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&outside));
    }
    inputs[0] = MemoryBatchCommit(table, first);
    inputs[1] = MemoryBatchCommit(table, last);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(inputs, 2));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, CohortCommitOrdinaryPlayOutsideCohortBlocksWithoutPartialCommit) { CheckCohortOutsideBlocker(true); }
TEST(CurrentSelectionMemory, CohortCommitOutsideBatchBlocksWithoutPartialCommit) { CheckCohortOutsideBlocker(false); }

TEST(CurrentSelectionMemory, CohortCommitMixedResetRefreshesFollowingBatchAndLegacySchedule) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    auto config = MemoryConfig();
    config.maxPendingEmitterPerRow = 8;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 legacy, native;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &legacy, 1280));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&legacy, 2));
    harken::PendingSelectionBatchV1 first, second, follower;
    auto request = MemoryRequest(program, 3);
    request.executeFrame = 320;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, request, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 4, 0, &native, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&native, 4));
    request = MemoryRequest(program, 5);
    request.executeFrame = 1000;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, request, second));
    request = MemoryRequest(program, 6);
    request.executeFrame = 1600;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, request, follower));
    const auto followerBefore = table.ReadPendingBatchMember(follower, 0);
    ASSERT_EQ(2u, followerBefore.seedProbe.resetEpoch);
    ASSERT_EQ(1u, table.ReadPendingBatchMember(second, 0).seedProbe.resetEpoch);
    harken::PendingSelectionBatchCommitV1 inputs[] = {MemoryBatchCommit(table, first), MemoryBatchCommit(table, second)};
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(inputs, 2));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    const auto followerAfter = table.ReadPendingBatchMember(follower, 0);
    EXPECT_EQ(followerBefore.ref.slot, followerAfter.ref.slot);
    EXPECT_EQ(followerBefore.ref.generation, followerAfter.ref.generation);
    EXPECT_EQ(followerBefore.ref.lifetime, followerAfter.ref.lifetime);
    EXPECT_EQ(2u, followerAfter.seedProbe.resetEpoch);
    EXPECT_GT(followerAfter.revision, followerBefore.revision);
    harken::PreparedSelectionPlayV1 hidden;
    std::uint32_t selected = 0;
    request.ticket = request.acceptedPostOrdinal = 7;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(request, &hidden, &selected));
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(hidden).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&hidden));
    harken::PreparedSelectionResetV1 late;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 8, 0, &late, 960));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&late, 8));
    request.ticket = request.acceptedPostOrdinal = 9;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(request, &hidden, &selected));
    EXPECT_EQ(3u, table.PreparedPlaySeedProbe(hidden).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&hidden));
    EXPECT_EQ(3u, table.ReadPendingBatchMember(follower, 0).seedProbe.resetEpoch);
    const auto finalInput = MemoryBatchCommit(table, follower);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(&finalInput, 1));
    table.ApplyDue(2000);
    table.ApplyDue(2000);
    EXPECT_EQ(3u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, CohortCommitEmptyProducerCancelsValidStalePrivateReplacement) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto request = MemoryRequest(program, 1);
    harken::PendingSelectionBatchV1 empty, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.IssueProducerPendingSelectionBatch(ProducerIdentity(request), nullptr, 0, &empty));
    harken::PendingSelectionReplacementV1 replacement;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.BeginPendingBatchReplacement(empty, 1, &replacement));
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&replacement, request, &selected));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 2), second));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CanInstallReplacement(replacement));
    harken::PendingSelectionBatchCommitV1 inputs[] = {MemoryBatchCommit(table, empty), MemoryBatchCommit(table, second)};
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(inputs, 2));
    EXPECT_FALSE(empty.Valid());
    EXPECT_FALSE(second.Valid());
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotFound, table.CancelReplacement(&replacement));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
    EXPECT_EQ(8u, table.FreePendingBatchCount());
}

TEST(CurrentSelectionMemory, CohortCommitPrivateResetBlocksWithoutConsumingAnyProducer) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 first, second;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 2), second));
    harken::PreparedSelectionResetV1 reset;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 3, 0, &reset));
    harken::PendingSelectionBatchCommitV1 inputs[] = {MemoryBatchCommit(table, first), MemoryBatchCommit(table, second)};
    EXPECT_EQ(harken::SelectionMemoryStatusV1::NotHead, table.CommitPendingBatches(inputs, 2));
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_TRUE(reset.Valid());
    EXPECT_EQ(5u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedReset(&reset));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(inputs, 2));
    EXPECT_EQ(2u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, CohortCommitBoundsAndEmptyInputDoNotTouchLiveHistory) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 1), batch));
    const auto input = MemoryBatchCommit(table, batch);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(nullptr, 0));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPendingBatches(nullptr, 1));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPendingBatches(&input, 9));
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(1u, table.PendingCount());
    EXPECT_TRUE(batch.Valid());
    EXPECT_EQ(7u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(&input, 1));
}

TEST(CurrentSelectionMemory, CohortCommitAnonymousBatchIsRejectedWithoutConsumption) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto request = MemoryRequest(program, 1);
    harken::PreparedSelectionPlayV1 prepared;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlayForPendingBatch(request, &prepared, &selected));
    harken::PreparedSelectionPlayV1* members[] = {&prepared};
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.MovePreparedPlaysToPending(members, 1, 1, &batch));
    const auto input = MemoryBatchCommit(table, batch);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::InvalidInput, table.CommitPendingBatches(&input, 1));
    EXPECT_TRUE(batch.Valid());
    EXPECT_EQ(1u, table.PendingCount());
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatch(&batch, input.revision));
}

TEST(CurrentSelectionMemory, CohortCommitRefreshesLegacyScheduledCacheBeforeLateAdmission) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    const auto initial = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, initial.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitHead(initial.delta));
    harken::PreparedSelectionResetV1 legacy, native;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReset(MemoryKey(), 2, 0, &legacy, 1280));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPreparedReset(&legacy, 2));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 3, 0, &native, 640));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&native, 3));
    auto request = MemoryRequest(program, 4);
    request.executeFrame = 1000;
    harken::PendingSelectionBatchV1 batch;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, request, batch));
    const auto input = MemoryBatchCommit(table, batch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(&input, 1));
    ASSERT_EQ(0u, table.PendingCount());
    request.ticket = request.acceptedPostOrdinal = 5;
    request.executeFrame = 1600;
    harken::PreparedSelectionPlayV1 hidden;
    std::uint32_t selected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &hidden, &selected));
    EXPECT_EQ(2u, table.PreparedPlaySeedProbe(hidden).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&hidden));
    harken::PreparedSelectionResetV1 late;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePendingReset(MemoryKey(), 6, 0, &late, 960));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.AcceptPreparedPendingReset(&late, 6));
    request.ticket = request.acceptedPostOrdinal = 7;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PreparePlay(request, &hidden, &selected));
    EXPECT_EQ(3u, table.PreparedPlaySeedProbe(hidden).resetEpoch);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPreparedPlay(&hidden));
    EXPECT_EQ(1u, table.Snapshot(MemoryKey()).resetEpoch);
    table.ApplyDue(2000);
    EXPECT_EQ(3u, table.Snapshot(MemoryKey()).resetEpoch);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, CohortCommitFailedLaterSelectionPreservesEarlierProducerAndHistory) {
    MemoryProgram program(harken::ContainerModeV1::Sequence);
    program.program.sequenceEndPolicy = harken::SequenceEndPolicyV1::StopAtEnd;
    harken::SelectionMemoryTableV1 table;
    auto config = MemoryConfig();
    config.maxPendingEmitterPerRow = 8;
    ASSERT_TRUE(table.Reset(config));
    harken::PendingSelectionBatchV1 first, second;
    auto request = MemoryRequest(program, 1);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, request, first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, MemoryRequest(program, 2), second));
    harken::PendingSelectionReplacementV1 candidate;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
        table.BeginPendingBatchReplacement(first, table.ReadPendingBatch(first).revision, &candidate));
    std::uint32_t selected = 0;
    for (std::uint32_t event = 0; event < 3; ++event) {
        request.eventIndex = event;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.PrepareReplacementPlay(&candidate, request, &selected));
    }
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.SealReplacement(&candidate, 3, 0));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.InstallReplacement(&candidate, &first));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::SequenceComplete, table.ReadPendingBatchMember(second, 0).delta.status);
    harken::PendingSelectionBatchCommitV1 inputs[] = {MemoryBatchCommit(table, first), MemoryBatchCommit(table, second)};
    EXPECT_EQ(harken::SelectionMemoryStatusV1::SequenceComplete, table.CanCommitPendingBatches(inputs, 2));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::SequenceComplete, table.CommitPendingBatches(inputs, 2));
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_EQ(0u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(4u, table.PendingCount());
    EXPECT_EQ(4u, table.FreeDeltaCount());
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CancelPendingBatch(&second));
    const auto valid = MemoryBatchCommit(table, first);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(&valid, 1));
    EXPECT_EQ(3u, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_TRUE(table.Snapshot(MemoryKey()).sequenceStopped);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, CohortCommitRandomRotationPreservesAuthenticHistoryAndSeeds) {
    MemoryProgram program(harken::ContainerModeV1::Random, harken::RandomDisciplineV1::Rotation);
    harken::SelectionMemoryTableV1 table, reference;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    ASSERT_TRUE(reference.Reset(MemoryConfig()));
    harken::PendingSelectionBatchV1 batches[3];
    harken::PendingSelectionBatchCommitV1 inputs[3];
    for (std::uint32_t index = 0; index < 3; ++index) {
        auto request = MemoryRequest(program, index + 1u);
        request.callerSeed += index;
        request.occurrenceOrdinal = index + 1u;
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, IssueMemoryProducerBatch(table, request, batches[index]));
        inputs[index] = MemoryBatchCommit(table, batches[index]);
        const auto expected = reference.AppendPlay(request);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, expected.status);
        EXPECT_EQ(reference.Delta(expected.delta).selectedChildOrdinal,
            table.ReadPendingBatchMember(batches[index], 0).delta.selectedChildOrdinal);
        EXPECT_EQ(index + 1u, table.ReadPendingBatchMember(batches[index], 0).seedProbe.selectionOrdinal);
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, reference.CommitHead(expected.delta));
    }
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.CommitPendingBatches(inputs, 3));
    EXPECT_EQ(reference.Snapshot(MemoryKey()).selectionOrdinal, table.Snapshot(MemoryKey()).selectionOrdinal);
    EXPECT_EQ(reference.Snapshot(MemoryKey()).rotationCursor, table.Snapshot(MemoryKey()).rotationCursor);
    std::array<std::uint32_t, 3> actual{}, expected{};
    EXPECT_EQ(reference.CopyPermutation(MemoryKey(), expected.data(), expected.size()),
        table.CopyPermutation(MemoryKey(), actual.data(), actual.size()));
    EXPECT_EQ(expected, actual);
    actual = expected = {};
    EXPECT_EQ(reference.CopyRecent(MemoryKey(), expected.data(), expected.size()),
        table.CopyRecent(MemoryKey(), actual.data(), actual.size()));
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(8u, table.FreeDeltaCount());
}

TEST(CurrentSelectionMemory, RotationAndSequenceStateRemainTransactional) {
    MemoryProgram rotation(harken::ContainerModeV1::Random,
                           harken::RandomDisciplineV1::Rotation);
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    std::array<harken::SelectionDeltaRefV1, 3> deltas{};
    std::array<std::uint32_t, 3> selected{};
    for (std::uint64_t ticket = 1; ticket <= 3; ++ticket) {
        const auto append = table.AppendPlay(MemoryRequest(rotation, ticket));
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, append.status);
        deltas[ticket - 1] = append.delta;
        selected[ticket - 1] =
            table.Delta(append.delta).selectedChildOrdinal;
    }
    std::sort(selected.begin(), selected.end());
    EXPECT_EQ((std::array<std::uint32_t, 3>{2, 5, 9}), selected);
    for (const auto delta : deltas) {
        ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
                  table.CommitHead(delta));
    }
    auto snapshot = table.Snapshot(MemoryKey());
    EXPECT_EQ(3u, snapshot.selectionOrdinal);
    EXPECT_EQ(3u, snapshot.rotationCursor);
    EXPECT_EQ(3u, snapshot.permutationCount);

    MemoryProgram sequence(harken::ContainerModeV1::Sequence);
    sequence.program.initialSequenceCursor = 1;
    sequence.program.sequenceEndPolicy = harken::SequenceEndPolicyV1::StopAtEnd;
    auto sequenceKey = MemoryKey();
    sequenceKey.containerStableId = harkentest::ObjectId(101);
    sequenceKey.programSemanticFingerprint = 201;
    sequence.program.containerStableId = harkentest::ObjectId(101);
    sequence.program.programSemanticFingerprint = 201;
    auto request = MemoryRequest(sequence, 4, sequenceKey);
    const auto middle = table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, middle.status);
    EXPECT_EQ(9u, table.Delta(middle.delta).selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(middle.delta));
    request.ticket = 5;
    request.acceptedPostOrdinal = 5;
    const auto last = table.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, last.status);
    EXPECT_EQ(2u, table.Delta(last.delta).selectedChildOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitHead(last.delta));
    request.ticket = 6;
    request.acceptedPostOrdinal = 6;
    EXPECT_EQ(harken::SelectionMemoryStatusV1::SequenceComplete,
              table.AppendPlay(request).status);
    snapshot = table.Snapshot(sequenceKey);
    EXPECT_EQ(2u, snapshot.selectionOrdinal);
    EXPECT_TRUE(snapshot.sequenceStopped);
}

TEST(CurrentSelectionMemory, PreparedPlaySeedProbeCopiesComputePlaySeedInput) {
    MemoryProgram program;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(MemoryConfig()));
    auto firstRequest = MemoryRequest(program, 1);
    firstRequest.callerSeed = UINT64_C(5);
    harken::PreparedSelectionPlayV1 first;
    std::uint32_t firstSelected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PreparePlay(firstRequest, &first, &firstSelected));
    const auto firstProbe = table.PreparedPlaySeedProbe(first);
    ASSERT_TRUE(firstProbe.captured);
    EXPECT_EQ(UINT64_C(5), firstProbe.derivedCallerSeed);
    EXPECT_EQ(UINT64_C(1), firstProbe.acceptedPostOrdinal);
    EXPECT_EQ(UINT64_C(1), firstProbe.selectionOrdinal);
    EXPECT_EQ(UINT64_C(0), firstProbe.resetEpoch);

    harken::SelectionSeedInputV1 expected{};
    expected.worldSemanticFingerprint = firstRequest.worldSemanticFingerprint;
    expected.programStableId = firstRequest.key.containerStableId;
    expected.programSemanticFingerprint =
        firstRequest.key.programSemanticFingerprint;
    expected.memoryScope = firstRequest.key.memoryScope;
    expected.emitterSlot = firstRequest.key.emitterSlot;
    expected.emitterGeneration = firstRequest.key.emitterGeneration;
    expected.resetEpoch = firstProbe.resetEpoch;
    expected.rootStableId = firstRequest.rootStableId;
    expected.callerSeed = firstProbe.derivedCallerSeed;
    expected.acceptedPostOrdinal = firstProbe.acceptedPostOrdinal;
    expected.selectionOrdinal = firstProbe.selectionOrdinal;
    expected.discipline = harken::SelectionDisciplineV1::Draw;
    std::uint64_t firstSeed = 0;
    ASSERT_TRUE(harken::ComputeSelectionSeedV1(expected, &firstSeed));
    harken::SplitMix64 firstStream(firstSeed);
    std::uint32_t reconstructed = 0;
    const harken::SelectionChildV1 children[] = {
        {2, 20, 2, 0},
        {5, 5, 1, 0},
        {9, 10, 3, 0},
    };
    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::SelectRandomDrawV1(children, 3, nullptr, 0, &firstStream,
                                        &reconstructed));
    EXPECT_EQ(firstSelected, reconstructed);

    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CommitPreparedPlayTransactional(&first, firstRequest.ticket));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, table.FinalizeCommittedPlay(&first));

    auto secondRequest = MemoryRequest(program, 2);
    secondRequest.callerSeed = UINT64_C(5);
    harken::PreparedSelectionPlayV1 second;
    std::uint32_t secondSelected = 0;
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.PreparePlay(secondRequest, &second, &secondSelected));
    const auto secondProbe = table.PreparedPlaySeedProbe(second);
    ASSERT_TRUE(secondProbe.captured);
    EXPECT_EQ(UINT64_C(5), secondProbe.derivedCallerSeed);
    EXPECT_EQ(UINT64_C(2), secondProbe.acceptedPostOrdinal);
    EXPECT_EQ(UINT64_C(2), secondProbe.selectionOrdinal);
    EXPECT_EQ(UINT64_C(0), secondProbe.resetEpoch);
    EXPECT_NE(firstProbe.acceptedPostOrdinal, secondProbe.acceptedPostOrdinal);
    EXPECT_NE(firstProbe.selectionOrdinal, secondProbe.selectionOrdinal);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.CancelPreparedPlay(&second));
    EXPECT_FALSE(table.PreparedPlaySeedProbe(second).captured);
}

TEST(CurrentSelectionMemory, ScopeCapacityRetirementAndQuarantineAreExact) {
    MemoryProgram program;
    auto config = MemoryConfig();
    config.maxEmitters = 1;
    config.maxGlobalRows = 1;
    config.maxEmitterRowsPerEmitter = 1;
    config.maxPendingDeltasTotal = 1;
    config.maxPendingGlobalPerRow = 1;
    config.maxPendingEmitterPerRow = 1;
    harken::SelectionMemoryTableV1 table;
    ASSERT_TRUE(table.Reset(config));
    ASSERT_TRUE(table.SetDeltaGenerationForTests(0, UINT32_MAX));

    const auto first = table.AppendPlay(MemoryRequest(program, 1));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, first.status);
    EXPECT_EQ(UINT32_MAX, first.delta.generation);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Capacity,
              table.AppendPlay(MemoryRequest(program, 2)).status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok,
              table.FailHead(first.delta));
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(0u, table.FreeDeltaCount());
    EXPECT_EQ(1u, table.QuarantinedDeltaCount());
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Capacity,
              table.AppendPlay(MemoryRequest(program, 3)).status);

    config.maxPendingDeltasTotal = 4;
    ASSERT_TRUE(table.Reset(config));
    auto emitterGenerationOne = MemoryKey();
    const auto emitter = table.AppendPlay(
        MemoryRequest(program, 1, emitterGenerationOne));
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, emitter.status);
    auto emitterGenerationTwo = MemoryKey(harken::MemoryScopeV1::Emitter, 2);
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Capacity,
              table.AppendPlay(
                       MemoryRequest(program, 2, emitterGenerationTwo))
                  .status);
    table.RetireEmitter(0, 1);
    EXPECT_FALSE(table.Delta(emitter.delta).live);
    const auto replacement = table.AppendPlay(
        MemoryRequest(program, 3, emitterGenerationTwo));
    EXPECT_EQ(harken::SelectionMemoryStatusV1::Ok, replacement.status);
    EXPECT_EQ(1u, table.RowCount());
}

TEST(CurrentSelectionMemory, PreparedShapesPreserveOtherHoldsAndOrdinaryAdmissions) {
    using Status = harken::SelectionMemoryStatusV1;
    using Table = harken::SelectionMemoryTableV1;
    auto config = MemoryConfig();
    config.maxGlobalRows = 0;
    config.maxPendingDeltasTotal = 4;
    Table table;
    ASSERT_TRUE(table.Reset(config));
    const auto emptyBytes = table.StorageBytes();
    Table::BackingPreparation small, large;
    table.SetCommitPageLimitForTests(0);
    EXPECT_FALSE(table.PrepareBacking({2, 2, 1, {1, 0, 1, 3}}, &small));
    EXPECT_FALSE(small.Valid());
    EXPECT_EQ(0u, table.RowCount());
    EXPECT_EQ(config.maxPendingDeltasTotal, table.FreeDeltaCount());
    table.SetCommitPageLimitForTests(SIZE_MAX);

    MemoryProgram draw;
    harken::SelectionPathNodeV1 path{};
    path.kind = harken::PlayableKindV1::Container;
    path.objectId = harkentest::ObjectId(401);
    auto request = MemoryRequest(draw, 1);
    request.path = &path;
    request.pathCount = 1;
    const auto active = table.AppendPlay(request);
    ASSERT_EQ(Status::Ok, active.status);
    const auto selected = table.Delta(active.delta).selectedChildOrdinal;
    ASSERT_TRUE(table.PrepareBacking({2, 2, 1, {1, 0, 1, 3}}, &small));
    ASSERT_TRUE(table.PrepareBacking({2, 2, 1, {1, 3, 2, 3}}, &large));
    Table::BackingPreparation moved(std::move(small));
    request.ticket = request.acceptedPostOrdinal = 2;
    const auto ordinary = table.AppendPlay(request);
    ASSERT_EQ(Status::Ok, ordinary.status);
    ASSERT_EQ(Status::Ok, table.CancelDelta(ordinary.delta));
    EXPECT_EQ(selected, table.Delta(active.delta).selectedChildOrdinal);

    MemoryProgram rotation(harken::ContainerModeV1::Random, harken::RandomDisciplineV1::Rotation);
    auto key = MemoryKey();
    SetSecondMemoryProgram(rotation, key);
    std::array<harken::SelectionPathNodeV1, 2> nestedPath{path, path};
    auto replacement = MemoryRequest(rotation, 3, key);
    replacement.path = nestedPath.data();
    replacement.pathCount = static_cast<std::uint32_t>(nestedPath.size());
    const auto preparedBytes = table.StorageBytes();
    table.SetCommitPageLimitForTests(0);
    {
        auto use = large.Use();
        harken::PreparedSelectionPlayV1 pending;
        std::uint32_t ordinal = 0;
        ASSERT_EQ(Status::Ok, table.PreparePlayForPendingBatch(replacement, &pending, &ordinal));
        ASSERT_EQ(Status::Ok, table.CancelPreparedPlay(&pending));
    }
    EXPECT_EQ(preparedBytes, table.StorageBytes());
    ASSERT_EQ(Status::Ok, table.FailHead(active.delta));
    EXPECT_EQ(config.maxPendingDeltasTotal, table.FreeDeltaCount());
    moved.Release();
    large.Release();
    ASSERT_TRUE(table.Reset(config));
    EXPECT_EQ(emptyBytes, table.StorageBytes());
}

TEST(CurrentSelectionMemory, ContentSizedBackingPreservesSelectionAndCancellationAcrossDisciplines) {
    using Status = harken::SelectionMemoryStatusV1;
    using Table = harken::SelectionMemoryTableV1;
    for (const auto count : {65u, 257u}) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            SCOPED_TRACE(count);
            SCOPED_TRACE(mode);
            MemoryProgram fixture;
            std::vector<harken::ContainerChildProgramViewV2> children(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                children[i].stableChildOrdinal = 7 + 2 * i;
                children[i].authoredStableOrder = count - i;
                children[i].frequencyWeight = mode == 0 ? 1 + i % 5 : 0;
                children[i].orderWeight = mode == 1 ? 1 + i % 5 : 0;
            }
            fixture.program.children = children.data();
            fixture.program.childCount = count;
            fixture.program.mode = mode == 2 ? harken::ContainerModeV1::Sequence : harken::ContainerModeV1::Random;
            fixture.program.randomDiscipline = mode == 1 ? harken::RandomDisciplineV1::Rotation : harken::RandomDisciplineV1::Draw;
            fixture.program.avoidLastN = mode == 2 ? 0 : harken::kHardSelectionAvoidCapacity;
            auto config = MemoryConfig();
            config.maxChildren = harken::kHardContainerChildCapacity;
            config.maxAvoidLastN = harken::kHardSelectionAvoidCapacity;
            Table table, reference;
            ASSERT_TRUE(table.Reset(config));
            ASSERT_TRUE(reference.Reset(MemoryConfig()));
            EXPECT_EQ(reference.StorageBytes(), table.StorageBytes());
            ASSERT_TRUE(reference.Reset(config));
            Table::BackingPreparation backing;
            ASSERT_TRUE(table.PrepareBacking({1, 2, 1,
                {fixture.program.avoidLastN, mode == 1 ? count : 0u, 0, count}}, &backing));
            const auto preparedBytes = table.StorageBytes();
            std::vector<std::uint32_t> history;
            history.reserve(count);
            const auto iterations = mode == 2 ? 4u : count;
            for (std::uint32_t i = 0; i < iterations; ++i) {
                const auto request = MemoryRequest(fixture, i + 1u);
                const auto expected = reference.AppendPlay(request);
                ASSERT_EQ(Status::Ok, expected.status);
                auto use = backing.Use();
                const auto cancelled = table.AppendPlay(request);
                ASSERT_EQ(Status::Ok, cancelled.status);
                ASSERT_EQ(Status::Ok, table.CancelDelta(cancelled.delta));
                const auto actual = table.AppendPlay(request);
                ASSERT_EQ(Status::Ok, actual.status);
                const auto ordinal = table.Delta(actual.delta).selectedChildOrdinal;
                EXPECT_EQ(reference.Delta(expected.delta).selectedChildOrdinal, ordinal);
                if (mode == 2) EXPECT_EQ(children[count - i - 1].stableChildOrdinal, ordinal);
                else {
                    const auto recent = std::min<std::size_t>(history.size(), fixture.program.avoidLastN);
                    EXPECT_EQ(history.end(), std::find(history.end() - recent, history.end(), ordinal));
                    history.push_back(ordinal);
                }
                ASSERT_EQ(Status::Ok, table.CommitHead(actual.delta));
                ASSERT_EQ(Status::Ok, reference.CommitHead(expected.delta));
                EXPECT_EQ(preparedBytes, table.StorageBytes());
            }
            EXPECT_EQ(iterations, table.Snapshot(MemoryKey()).selectionOrdinal);
            EXPECT_EQ(0u, table.PendingCount());
        }
    }
}

}
