// VariableContractArtifactV1: vintage-1 Persistent-only binary/JSON, 32-byte
// execution prefix, no Argument keys, deterministic encode.
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_project_schema/VariableContract.h"

#include "harken/core/VariableExecutionFingerprint.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
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

std::uint64_t ExpectedExecution(const VariableContractRecordV1& record) {
    std::vector<std::int32_t> anchors;
    if (record.execution.kind == ControlValueKindV1::Discrete)
        for (const auto& label : record.catalog.labels) anchors.push_back(label.anchor);
    const std::int32_t* pointer = anchors.empty() ? nullptr : anchors.data();
    std::uint64_t fingerprint = 0;
    EXPECT_TRUE(harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ToRuntime(record.execution.variableId),
        static_cast<std::uint8_t>(record.execution.kind),
        static_cast<std::uint8_t>(record.execution.scopePolicy),
        static_cast<std::uint8_t>(record.execution.writers),
        record.execution.range.minimum, record.execution.range.maximum,
        record.execution.range.defaultValue, pointer,
        static_cast<std::uint32_t>(anchors.size()), &fingerprint));
    return fingerprint;
}

}  // namespace

TEST(VariableContractArtifact, PersistentOnlyUuidVintage) {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(TwoVariableFixture(), &bytes));
    ASSERT_GE(bytes.size(), kVariableContractHeaderBytesV1 +
                                kVariableCapacityBytesV1);
    EXPECT_EQ('V', bytes[0]);
    EXPECT_EQ('N', bytes[1]);
    EXPECT_EQ('V', bytes[2]);
    EXPECT_EQ('C', bytes[3]);
    EXPECT_EQ(kVariableContractSchemaVersionV1, ReadU32(bytes, 4) & 0xffffu);
    EXPECT_EQ(0u, (ReadU32(bytes, 4) >> 16) & 0xffffu);
    EXPECT_EQ(2u, ReadU32(bytes, 32));
    EXPECT_EQ(0u, ReadU32(bytes, 36));

    VariableContractArtifactV1 decoded;
    ASSERT_TRUE(DecodeVariableContractArtifactV1(bytes.data(), bytes.size(),
                                                 &decoded));
    EXPECT_EQ(kVariableContractSchemaVersionV1, decoded.schemaVersion);
    EXPECT_EQ(0x1111111111111111ull, decoded.projectBuildId);
    ASSERT_EQ(2u, decoded.variables.size());
    EXPECT_EQ(harkentest::StudioObjectId(7u), decoded.variables[0].execution.variableId);
    EXPECT_EQ(ControlValueKindV1::Discrete, decoded.variables[0].execution.kind);
    EXPECT_EQ("health", decoded.variables[0].catalog.codeName);
    EXPECT_EQ("Health", decoded.variables[0].catalog.displayName);
    ASSERT_EQ(2u, decoded.variables[0].catalog.labels.size());
    EXPECT_EQ(0, decoded.variables[0].catalog.labels[0].anchor);
    EXPECT_EQ("off", decoded.variables[0].catalog.labels[0].name);
    EXPECT_EQ(8, decoded.variables[0].catalog.labels[1].anchor);
    EXPECT_EQ(0x123456789abcdef0ull,
              decoded.variables[0].catalog.ownerInitPackId);
    EXPECT_EQ(harkentest::StudioObjectId(11u), decoded.variables[1].execution.variableId);
    EXPECT_EQ(ControlValueKindV1::Continuous,
              decoded.variables[1].execution.kind);
    EXPECT_TRUE(decoded.variables[1].catalog.labels.empty());
    EXPECT_EQ(ExpectedExecution(DiscreteHealth()),
              decoded.variables[0].execution.executionFingerprint);
    EXPECT_EQ(ExpectedExecution(ContinuousVolume()),
              decoded.variables[1].execution.executionFingerprint);
    EXPECT_EQ(2u, decoded.capacity.definitionCount);
    EXPECT_EQ(64u, decoded.capacity.recommendedMaxEmitters);
}

TEST(VariableContractArtifact, NoArgumentKeys) {
    std::string json;
    ASSERT_TRUE(
        EncodeVariableContractArtifactJsonV1(TwoVariableFixture(), &json));
    EXPECT_EQ(std::string::npos, json.find("actionArguments"));
    EXPECT_EQ(std::string::npos, json.find("actionSignatures"));
    EXPECT_EQ(0u, json.find("{\"schemaVersion\":"));
    EXPECT_LT(json.find("schemaVersion"), json.find("projectBuildId"));
    EXPECT_LT(json.find("projectBuildId"),
              json.find("executionCatalogFingerprint"));
    EXPECT_LT(json.find("executionCatalogFingerprint"),
              json.find("sdkCatalogFingerprint"));
    EXPECT_LT(json.find("sdkCatalogFingerprint"), json.find("variables"));
    EXPECT_LT(json.find("\"variables\""), json.find("\"capacity\""));
}

TEST(VariableContractArtifact, SmoothLabelIdentityRoundTripsAndAffectsOnlyCatalog) {
    VariableContractArtifactV1 source;
    source.variables = {ContinuousVolume()};
    source.variables[0].catalog.labels = {{0, "None", harkentest::StudioObjectId(90)}};
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(source, &bytes));
    VariableContractArtifactV1 decoded;
    ASSERT_TRUE(DecodeVariableContractArtifactV1(bytes.data(), bytes.size(), &decoded));
    ASSERT_EQ(decoded.variables[0].catalog.labels.size(), 1u);
    EXPECT_EQ(decoded.variables[0].catalog.labels[0].labelId, harkentest::StudioObjectId(90));
    EXPECT_EQ(decoded.variables[0].execution.executionFingerprint, ExpectedExecution(ContinuousVolume()));
    const auto oldCatalog = decoded.sdkCatalogFingerprint;
    source.variables[0].catalog.labels[0].name = "Ready";
    ASSERT_TRUE(EncodeVariableContractArtifactV1(source, &bytes));
    ASSERT_TRUE(DecodeVariableContractArtifactV1(bytes.data(), bytes.size(), &decoded));
    EXPECT_NE(decoded.sdkCatalogFingerprint, oldCatalog);
    source.variables[0].catalog.labels.push_back({1, "Ready", harkentest::StudioObjectId(91)});
    EXPECT_FALSE(EncodeVariableContractArtifactV1(source, &bytes));
    source.variables[0].catalog.labels.back().name = "Other";
    source.variables[0].catalog.labels.back().anchor = 7;
    EXPECT_FALSE(EncodeVariableContractArtifactV1(source, &bytes));
}

TEST(VariableContractArtifact, DeterministicBytes) {
    auto firstInput = TwoVariableFixture();
    auto secondInput = TwoVariableFixture();
    secondInput.variables = {DiscreteHealth(), ContinuousVolume()};
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    std::vector<std::uint8_t> swapped;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(firstInput, &first));
    ASSERT_TRUE(EncodeVariableContractArtifactV1(firstInput, &second));
    ASSERT_TRUE(EncodeVariableContractArtifactV1(secondInput, &swapped));
    EXPECT_EQ(first, second);
    EXPECT_EQ(first, swapped);
    std::string jsonA;
    std::string jsonB;
    ASSERT_TRUE(EncodeVariableContractArtifactJsonV1(firstInput, &jsonA));
    ASSERT_TRUE(EncodeVariableContractArtifactJsonV1(secondInput, &jsonB));
    EXPECT_EQ(jsonA, jsonB);
}

TEST(VariableContractArtifact, IntegerIdentitySchemaIsRejected) {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(TwoVariableFixture(), &bytes));
    bytes[4] = 1;
    bytes[5] = 0;
    VariableContractArtifactV1 decoded;
    decoded.schemaVersion = 99;
    EXPECT_FALSE(DecodeVariableContractArtifactV1(bytes.data(), bytes.size(),
                                                  &decoded));
    EXPECT_EQ(99u, decoded.schemaVersion);
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

TEST(VariableContractArtifact, NameOnlyCatalogChangeLoads) {
    auto named = DiscreteHealth();
    named.catalog.codeName = "health";
    named.catalog.displayName = "Health";
    auto renamed = named;
    renamed.catalog.codeName = "hp";
    renamed.catalog.displayName = "Hit Points";
    VariableContractArtifactV1 first;
    first.schemaVersion = kVariableContractSchemaVersionV1;
    first.variables.push_back(named);
    VariableContractArtifactV1 second = first;
    second.variables[0] = renamed;
    std::string jsonA;
    std::string jsonB;
    ASSERT_TRUE(EncodeVariableContractArtifactJsonV1(first, &jsonA));
    ASSERT_TRUE(EncodeVariableContractArtifactJsonV1(second, &jsonB));
    VariableContractArtifactV1 decodedA;
    VariableContractArtifactV1 decodedB;
    std::vector<std::uint8_t> bytesA;
    std::vector<std::uint8_t> bytesB;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(first, &bytesA));
    ASSERT_TRUE(EncodeVariableContractArtifactV1(second, &bytesB));
    ASSERT_TRUE(DecodeVariableContractArtifactV1(bytesA.data(), bytesA.size(),
                                                 &decodedA));
    ASSERT_TRUE(DecodeVariableContractArtifactV1(bytesB.data(), bytesB.size(),
                                                 &decodedB));
    ASSERT_EQ(1u, decodedA.variables.size());
    ASSERT_EQ(1u, decodedB.variables.size());
    EXPECT_EQ(decodedA.variables[0].execution.executionFingerprint,
              decodedB.variables[0].execution.executionFingerprint);
    EXPECT_EQ(ExpectedExecution(named),
              decodedA.variables[0].execution.executionFingerprint);
    EXPECT_EQ(decodedA.executionCatalogFingerprint,
              decodedB.executionCatalogFingerprint);
    EXPECT_NE(decodedA.sdkCatalogFingerprint, decodedB.sdkCatalogFingerprint);
    EXPECT_NE(jsonA, jsonB);
}

TEST(VariableContractArtifact, ExecutionRecordIncludesFullUuid) {
    EXPECT_EQ(40u, kVariableExecutionRecordBytesV1);
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeVariableContractArtifactV1(TwoVariableFixture(), &bytes));
    ASSERT_GT(bytes.size(), kVariableContractHeaderBytesV1 +
                                kVariableExecutionRecordBytesV1 + 4);
    EXPECT_EQ(harkentest::StudioObjectId(7), harkenstudio::ObjectIdFromBytes(
        std::span<const std::uint8_t, 16>(bytes.data() + kVariableContractHeaderBytesV1, 16)));
    const std::size_t catalog = kVariableContractHeaderBytesV1 +
                                kVariableExecutionRecordBytesV1;
    EXPECT_EQ(6u, ReadU32(bytes, catalog));
    EXPECT_EQ(0, std::memcmp(bytes.data() + catalog + 4, "health", 6));
}

}  // namespace harkenprojectschema
