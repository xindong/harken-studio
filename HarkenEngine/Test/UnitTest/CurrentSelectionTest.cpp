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

}
