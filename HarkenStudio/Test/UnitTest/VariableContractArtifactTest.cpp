#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_project_schema/VariableContract.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace harkenprojectschema {
namespace {

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint64_t>(ReadU32(bytes, at)) |
           (static_cast<std::uint64_t>(ReadU32(bytes, at + 4)) << 32);
}

void WriteU32(std::vector<std::uint8_t>* bytes, std::size_t at,
              std::uint32_t value) {
    (*bytes)[at] = static_cast<std::uint8_t>(value);
    (*bytes)[at + 1] = static_cast<std::uint8_t>(value >> 8);
    (*bytes)[at + 2] = static_cast<std::uint8_t>(value >> 16);
    (*bytes)[at + 3] = static_cast<std::uint8_t>(value >> 24);
}

void WriteU64(std::vector<std::uint8_t>* bytes, std::size_t at,
              std::uint64_t value) {
    WriteU32(bytes, at, static_cast<std::uint32_t>(value));
    WriteU32(bytes, at + 4, static_cast<std::uint32_t>(value >> 32));
}

std::size_t SkipPaddedBytes(const std::vector<std::uint8_t>& bytes,
                            std::size_t at) {
    const std::uint32_t size = ReadU32(bytes, at);
    return at + sizeof(std::uint32_t) + size +
           (4u - (size % 4u)) % 4u;
}

std::size_t FirstLabelCountOffset(const std::vector<std::uint8_t>& bytes) {
    std::size_t at =
        kVariableContractHeaderBytesV1 + kVariableExecutionRecordBytesV1;
    for (int i = 0; i < 4; ++i) {
        at = SkipPaddedBytes(bytes, at);
    }
    return at + sizeof(std::int32_t) + 2u * sizeof(std::uint8_t) +
           sizeof(std::uint16_t) + sizeof(harkenstudio::ObjectId);
}

VariableContractRecordV1 DiscreteHealth() {
    VariableContractRecordV1 record;
    record.execution.variableId = harkentest::StudioObjectId(7);
    record.execution.kind = ControlValueKindV1::Discrete;
    record.execution.scopePolicy = VariableScopePolicyV1::GlobalWithEmitterOverride;
    record.execution.writers = VariableWriterGame;
    record.execution.range = {0.0f, 8.0f, 0.0f};
    record.catalog.codeName = "health";
    record.catalog.displayName = "Health";
    record.catalog.labels = {{0, "off", harkentest::StudioObjectId(70)},
                             {8, "full", harkentest::StudioObjectId(71)}};
    record.catalog.ownerInitPackId = 0x123456789abcdef0ull;
    return record;
}

VariableContractRecordV1 ContinuousVolume() {
    VariableContractRecordV1 record;
    record.execution.variableId = harkentest::StudioObjectId(11);
    record.execution.kind = ControlValueKindV1::Continuous;
    record.execution.scopePolicy =
        VariableScopePolicyV1::GlobalWithEmitterOverride;
    record.execution.writers = static_cast<VariableWriterMaskV1>(
        VariableWriterGame | VariableWriterAction);
    record.execution.range = {0.0f, 6.0f, 0.0f};
    record.catalog.codeName = "volume";
    record.catalog.displayName = "Volume";
    record.catalog.unit = "dB";
    record.catalog.precision = 1;
    record.catalog.displayScale = 1;
    return record;
}

VariableContractArtifactV1 TwoVariableFixture() {
    VariableContractArtifactV1 artifact;
    artifact.schemaVersion = kVariableContractSchemaVersionV1;
    artifact.projectBuildId = 0x1111111111111111ull;
    artifact.variables.push_back(ContinuousVolume());
    artifact.variables.push_back(DiscreteHealth());
    artifact.capacity.definitionCount = 2;
    artifact.capacity.maxReferencedPerContentPack = 16;
    artifact.capacity.maxVariableBindings = 32;
    artifact.capacity.maxStaticEmitterOverrides = 8;
    artifact.capacity.recommendedMaxEmitters = 64;
    artifact.capacity.recommendedEmitterVariableSlots = 4;
    artifact.capacity.recommendedPendingRows = 128;
    return artifact;
}

}

TEST(VariableContractArtifact, ArtifactFingerprintMismatchIsRejected) {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(TwoVariableFixture(), &bytes));

    auto executionMismatch = bytes;
    WriteU64(&executionMismatch, 16, ReadU64(executionMismatch, 16) ^ 1u);
    VariableContractArtifactV1 decoded;
    EXPECT_FALSE(DecodeVariableContractArtifactV1(
        executionMismatch.data(), executionMismatch.size(), &decoded));

    auto sdkMismatch = bytes;
    WriteU64(&sdkMismatch, 24, ReadU64(sdkMismatch, 24) ^ 1u);
    EXPECT_FALSE(DecodeVariableContractArtifactV1(
        sdkMismatch.data(), sdkMismatch.size(), &decoded));
}

TEST(VariableContractArtifact, OversizedVariableCountIsRejected) {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(TwoVariableFixture(), &bytes));
    bytes.resize(kVariableContractHeaderBytesV1);
    WriteU32(&bytes, 32, std::numeric_limits<std::uint32_t>::max());
    VariableContractArtifactV1 decoded;
    EXPECT_FALSE(DecodeVariableContractArtifactV1(bytes.data(), bytes.size(),
                                                  &decoded));
}

TEST(VariableContractArtifact, OversizedLabelCountIsRejected) {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(TwoVariableFixture(), &bytes));
    WriteU32(&bytes, FirstLabelCountOffset(bytes),
             std::numeric_limits<std::uint32_t>::max());
    VariableContractArtifactV1 decoded;
    EXPECT_FALSE(DecodeVariableContractArtifactV1(bytes.data(), bytes.size(),
                                                  &decoded));
}

}
