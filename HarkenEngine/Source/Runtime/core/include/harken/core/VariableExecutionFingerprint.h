#ifndef HARKEN_CORE_VARIABLE_EXECUTION_FINGERPRINT_H
#define HARKEN_CORE_VARIABLE_EXECUTION_FINGERPRINT_H

#include <harken/ObjectId.h>
#include "harken/core/MediaContentRevision.h"
#include "harken/core/VariableLabel.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <span>

namespace harken {

inline constexpr char kVariableExecutionDomain[] = "HARKEN-VARIABLE-EXECUTION-V1";
inline constexpr char kVariableCatalogDomain[] = "HARKEN-VARIABLE-CATALOG-V1";
inline constexpr char kVariableExecutionCatalogDomain[] =
    "HARKEN-VARIABLE-EXECUTION-CATALOG-V1";
inline constexpr char kVariableSdkCatalogDomain[] =
    "HARKEN-VARIABLE-SDK-CATALOG-V1";

inline std::uint64_t ComputeVariableDefinitionFingerprintV1(
    VnObjectId variableId, std::uint32_t kind, std::uint32_t scope,
    float rangeMin, float rangeMax, float defaultValue) noexcept {
    constexpr char domain[] = "HARKEN-VARIABLE-DEFINITION-V1";
    MediaContentRevisionHasherV1 hash(domain, sizeof(domain));
    hash.AddObjectId(variableId);
    hash.AddU32(kind);
    hash.AddU32(scope);
    hash.AddU32(std::bit_cast<std::uint32_t>(rangeMin));
    hash.AddU32(std::bit_cast<std::uint32_t>(rangeMax));
    hash.AddU32(std::bit_cast<std::uint32_t>(defaultValue));
    return hash.Finish();
}

inline bool CanonicalF32Bits(float value, std::uint32_t* outBits) noexcept {
    if (outBits == nullptr) {
        return false;
    }
    if (!std::isfinite(value) || (value == 0.0f && std::signbit(value))) {
        return false;
    }
    *outBits = std::bit_cast<std::uint32_t>(value);
    return true;
}

inline bool ComputeVariableExecutionFingerprintV1(
    VnObjectId variableId, std::uint8_t kind, std::uint8_t scopePolicy,
    std::uint8_t writers, float rangeMin, float rangeMax, float defaultValue,
    const std::int32_t* discreteAnchorsSortedUnique,
    std::uint32_t discreteAnchorCount,
    std::uint64_t* outFingerprint) noexcept {
    if (outFingerprint == nullptr || variableId == VnObjectId{} || kind > 1 ||
        scopePolicy != 1 || writers < 1 || writers > 3) {
        return false;
    }

    std::uint32_t minBits = 0;
    std::uint32_t maxBits = 0;
    std::uint32_t defaultBits = 0;
    if (!CanonicalF32Bits(rangeMin, &minBits) ||
        !CanonicalF32Bits(rangeMax, &maxBits) ||
        !CanonicalF32Bits(defaultValue, &defaultBits) || rangeMin > rangeMax ||
        defaultValue < rangeMin || defaultValue > rangeMax) {
        return false;
    }

    if (kind == 1) {
        if (discreteAnchorCount != 0) {
            return false;
        }
    } else if (discreteAnchorCount > 0 &&
               discreteAnchorsSortedUnique == nullptr) {
        return false;
    } else {
        constexpr std::int32_t kExactIntBound = 16777216;
        for (std::uint32_t i = 0; i < discreteAnchorCount; ++i) {
            const std::int32_t anchor = discreteAnchorsSortedUnique[i];
            if (anchor < -kExactIntBound || anchor > kExactIntBound) {
                return false;
            }
            const float anchorValue = static_cast<float>(anchor);
            if (anchorValue < rangeMin || anchorValue > rangeMax) {
                return false;
            }
            if (i != 0 && anchor <= discreteAnchorsSortedUnique[i - 1]) {
                return false;
            }
        }
    }

    MediaContentRevisionHasherV1 hash(kVariableExecutionDomain,
                                      sizeof(kVariableExecutionDomain));
    const std::uint8_t identityClass = 1;
    hash.AddBytes(&identityClass, sizeof(identityClass));
    hash.AddObjectId(variableId);
    hash.AddBytes(&kind, sizeof(kind));
    hash.AddBytes(&scopePolicy, sizeof(scopePolicy));
    hash.AddBytes(&writers, sizeof(writers));
    hash.AddU32(minBits);
    hash.AddU32(maxBits);
    hash.AddU32(defaultBits);
    hash.AddU32(discreteAnchorCount);
    for (std::uint32_t i = 0; i < discreteAnchorCount; ++i) {
        hash.AddU32(
            static_cast<std::uint32_t>(discreteAnchorsSortedUnique[i]));
    }
    *outFingerprint = hash.Finish();
    return true;
}

struct VariableCatalogFingerprintRowV1 {
    VnObjectId variableId{};
    std::uint64_t fingerprint = 0;
};

inline std::uint64_t ComputeVariableLabelMappingFingerprintV1(
    VnObjectId variableId, std::span<const VariableLabelDefinitionV1> labels) noexcept {
    constexpr char domain[] = "HARKEN-VARIABLE-LABEL-MAPPING-V1";
    MediaContentRevisionHasherV1 hash(domain, sizeof(domain));
    hash.AddObjectId(variableId);
    hash.AddU32(static_cast<std::uint32_t>(labels.size()));
    for (const auto& label : labels) {
        hash.AddObjectId(label.labelId);
        hash.AddU32(static_cast<std::uint32_t>(label.anchor));
    }
    return hash.Finish();
}

inline std::uint64_t ComputeVariableExecutionCatalogFingerprintV1(
    const VariableCatalogFingerprintRowV1* rows, std::uint32_t count) noexcept {
    MediaContentRevisionHasherV1 hash(kVariableExecutionCatalogDomain,
                                      sizeof(kVariableExecutionCatalogDomain));
    for (std::uint32_t i = 0; i < count; ++i) {
        hash.AddObjectId(rows[i].variableId);
        hash.AddU64(rows[i].fingerprint);
    }
    return hash.Finish();
}

inline std::uint64_t ComputeVariableSdkCatalogFingerprintV1(
    const std::uint64_t* catalogFingerprints, std::uint32_t count) noexcept {
    MediaContentRevisionHasherV1 hash(kVariableSdkCatalogDomain,
                                      sizeof(kVariableSdkCatalogDomain));
    for (std::uint32_t i = 0; i < count; ++i) {
        hash.AddU64(catalogFingerprints[i]);
    }
    return hash.Finish();
}

} // namespace harken

#endif // HARKEN_CORE_VARIABLE_EXECUTION_FINGERPRINT_H
