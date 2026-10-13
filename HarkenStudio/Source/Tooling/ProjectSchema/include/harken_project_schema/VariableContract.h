#ifndef HARKEN_PROJECT_SCHEMA_VARIABLE_CONTRACT_H
#define HARKEN_PROJECT_SCHEMA_VARIABLE_CONTRACT_H

#include <harken_studio/ObjectId.h>
#include "harken_studio_build/VariableLabel.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace harkenprojectschema {

enum class ControlValueKindV1 : std::uint8_t {
    Discrete = 0,
    Continuous = 1
};

enum class VariableScopePolicyV1 : std::uint8_t {
    GlobalWithEmitterOverride = 1
};

enum VariableWriterMaskV1 : std::uint8_t {
    VariableWriterGame = 1u << 0,
    VariableWriterAction = 1u << 1
};

enum class ControlIdentityClassV1 : std::uint8_t {
    PersistentVariable = 1
};

struct ControlValueRangeV1 {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float defaultValue = 0.0f;
};

struct VariableExecutionDefinitionV1 {
    harkenstudio::ObjectId variableId{};
    ControlValueKindV1 kind = ControlValueKindV1::Discrete;
    VariableScopePolicyV1 scopePolicy = VariableScopePolicyV1::GlobalWithEmitterOverride;
    VariableWriterMaskV1 writers = VariableWriterGame;
    ControlValueRangeV1 range{};
    std::uint64_t executionFingerprint = 0;
};

struct VariablePolicyOverlayV1 {
    harkenstudio::ObjectId variableId{};
    VariableWriterMaskV1 writers = VariableWriterGame;
    std::uint8_t reserved0 = 0;
    std::uint16_t reserved1 = 0;
    std::uint64_t executionFingerprint = 0;
};

struct ControlDefinitionRequirementV1 {
    ControlIdentityClassV1 identityClass =
        ControlIdentityClassV1::PersistentVariable;
    harkenstudio::ObjectId stableId{};
    std::uint64_t expectedExecutionFingerprint = 0;
};

using VariableCatalogLabelV1 = harkenstudiobuild::VariableLabelDefinitionV1;

struct VariableCatalogBlockV1 {
    std::string codeName;
    std::string displayName;
    std::string description;
    std::string unit;
    std::int32_t precision = 0;
    std::uint8_t displayScale = 0;
    std::uint8_t deprecated01 = 0;
    harkenstudio::ObjectId replacementId{};
    std::vector<VariableCatalogLabelV1> labels;
    std::uint64_t ownerInitPackId = 0;
};

struct VariableContractRecordV1 {
    VariableExecutionDefinitionV1 execution;
    VariableCatalogBlockV1 catalog;
    std::uint64_t catalogFingerprint = 0;
};

struct VariableCapacityRecommendationV1 {
    std::uint32_t definitionCount = 0;
    std::uint32_t maxReferencedPerContentPack = 0;
    std::uint32_t maxVariableBindings = 0;
    std::uint32_t maxStaticEmitterOverrides = 0;
    std::uint32_t recommendedMaxEmitters = 0;
    std::uint32_t recommendedEmitterVariableSlots = 0;
    std::uint32_t recommendedPendingRows = 0;
    std::uint32_t reserved = 0;
};

struct VariableContractArtifactV1 {
    std::uint16_t schemaVersion = 3;
    std::uint64_t projectBuildId = 0;
    std::uint64_t executionCatalogFingerprint = 0;
    std::uint64_t sdkCatalogFingerprint = 0;
    std::vector<VariableContractRecordV1> variables;
    VariableCapacityRecommendationV1 capacity{};
};

inline constexpr std::size_t kVariableContractHeaderBytesV1 = 40;
inline constexpr std::size_t kVariableExecutionRecordBytesV1 = 40;
inline constexpr std::size_t kVariableCapacityBytesV1 = 32;
inline constexpr std::uint16_t kVariableContractSchemaVersionV1 = 3;

bool EncodeVariableContractArtifactV1(
    const VariableContractArtifactV1& artifact,
    std::vector<std::uint8_t>* outBytes);

bool DecodeVariableContractArtifactV1(const std::uint8_t* bytes,
                                      std::size_t byteCount,
                                      VariableContractArtifactV1* outArtifact);

bool EncodeVariableContractArtifactJsonV1(
    const VariableContractArtifactV1& artifact, std::string* outJson);

std::uint64_t ComputeVariableCatalogFingerprintV1(
    harkenstudio::ObjectId variableId, const VariableCatalogBlockV1& catalog);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_VARIABLE_CONTRACT_H
