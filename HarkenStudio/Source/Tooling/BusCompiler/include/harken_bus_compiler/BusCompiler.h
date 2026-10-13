#pragma once

#include <harken_studio/ObjectId.h>
#include <harken_studio/AudioValues.h>
#include <harken_project_schema/RuntimeIdentityIndex.h>
#include <harken_project_schema/CompileTargetProfile.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace harkenbuscompiler {

using BusProjectGenerationV1 = std::uint64_t;

using CompileTargetProfileV1 = harkenprojectschema::CompileTargetProfileV1;

inline constexpr std::uint64_t kBusRequiredCapabilityMaskV1 =
    (std::uint64_t{1} << 16) | (std::uint64_t{1} << 17)
    | (std::uint64_t{1} << 18) | (std::uint64_t{1} << 20)
    | (std::uint64_t{1} << 21) | (std::uint64_t{1} << 22);

enum class BusDiagnosticCodeV1 : std::uint32_t {
    Invalid = 0,
    MissingIdentity = 1,
    DuplicateIdentity = 2,
    MissingMaster = 3,
    MultipleMasterCandidates = 4,
    ReservedMasterCollision = 5,
    MissingParent = 6,
    MissingSendTarget = 7,
    SelfEdge = 8,
    CombinedTreeSendCycle = 9,
    NonFiniteScalar = 10,
    InvalidFaderPoint = 11,
    InvalidLayoutDownmix = 12,
    UnsupportedAuthoredProperty = 13,
    TargetCapabilityUnavailable = 14,
    BusCapacity = 15,
    PerBusSendCapacity = 16,
    TotalSendCapacity = 17,
    EffectCapacity = 18,
    CompiledByteCapacity = 19,
};

struct BusSourceLocationV1 {
    harkenprojectschema::AuthoringGuid authoringGuid{};
    std::string documentPath;
    std::string fieldPath;
};

struct BusDiagnosticV1 {
    BusDiagnosticCodeV1 code = BusDiagnosticCodeV1::Invalid;
    harkenstudio::ObjectId offendingId{};
    harkenstudio::ObjectId relatedId{};
    std::uint64_t projectGeneration = 0;
    BusSourceLocationV1 source;
    std::uint64_t detail = 0;
    std::uint64_t relatedDetail = 0;
};

struct BusCompilerCapacityV1 {
    std::uint32_t maxBusesIncludingOutputRoot = 32;
    std::uint32_t maxSendsPerBus = 4;
    std::uint32_t maxTotalSends = 128;
    std::uint32_t maxEffectsPerBus = 8;
    std::uint32_t maxDownmixRows = 32;
    std::uint64_t maxDownmixCoefficients = 2048;
    std::uint64_t maxCompiledBytes = UINT64_C(64) * 1024 * 1024;
};

struct BusAuthoringV1 {
    harkenstudio::ObjectId busId{};
    harkenstudio::ObjectId parentId{};
    float faderDb = 0.0f;
    std::uint32_t faderAfterSlot = 8;
    harkenstudio::ChannelLayout outputLayout = 0;
    BusSourceLocationV1 source;
    std::uint32_t effectFirst = 0;
    std::uint32_t effectCount = 0;
    std::uint32_t downmixFirst = 0;
    std::uint32_t downmixCount = 0;
    float makeUpGainDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
};

struct BusSendAuthoringV1 {
    harkenstudio::ObjectId sourceBusId{};
    harkenstudio::ObjectId targetBusId{};
    float sendLevelDb = 0.0f;
    BusSourceLocationV1 source;
};

struct BusEffectParameterAuthoringV1 {
    harkenstudio::ObjectId effectId{};
    std::uint32_t index = 0;
    float value = 0.0f;
    BusSourceLocationV1 source;
};

struct BusEffectAuthoringV1 {
    harkenstudio::ObjectId busId{};
    harkenstudio::ObjectId effectId{};
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 2;
    bool bypass = false;
    std::uint32_t parameterFirst = 0;
    std::uint32_t parameterCount = 0;
    BusSourceLocationV1 source;
};

struct BusDownmixAuthoringV1 {
    harkenstudio::ObjectId busId{};
    harkenstudio::ChannelLayout sourceLayout = 0;
    harkenstudio::ChannelLayout destinationLayout = 0;
    std::uint32_t coefficientFirst = 0;
    std::uint32_t coefficientCount = 0;
    BusSourceLocationV1 source;
};

struct CompiledBusV1 {
    harkenstudio::ObjectId busId{};
    harkenstudio::ObjectId parentId{};
    std::uint32_t feedsFirstOrdinal = 0;
    float faderDb = 0.0f;
    std::uint32_t faderAfterSlot = 8;
    harkenstudio::ChannelLayout outputLayout = 0;
    std::uint32_t effectFirst = 0;
    std::uint32_t effectCount = 0;
    std::uint32_t downmixFirst = 0;
    std::uint32_t downmixCount = 0;
    std::uint32_t sourceLocationIndex = 0;
    float makeUpGainDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
};

struct CompiledBusSendV1 {
    harkenstudio::ObjectId sourceBusId{};
    harkenstudio::ObjectId targetBusId{};
    float sendLevelDb = 0.0f;
    std::uint32_t sourceLocationIndex = 0;
};

struct CompiledBusEffectParameterV1 {
    std::uint32_t index = 0;
    float value = 0.0f;
    std::uint32_t sourceLocationIndex = 0;
};

struct CompiledBusEffectV1 {
    harkenstudio::ObjectId busId{};
    harkenstudio::ObjectId effectId{};
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 2;
    std::uint32_t flags = 0;
    std::uint32_t parameterFirst = 0;
    std::uint32_t parameterCount = 0;
    std::uint32_t sourceLocationIndex = 0;
};

struct CompiledBusDownmixV1 {
    harkenstudio::ObjectId busId{};
    harkenstudio::ChannelLayout sourceLayout = 0;
    harkenstudio::ChannelLayout destinationLayout = 0;
    std::uint32_t coefficientFirst = 0;
    std::uint32_t coefficientCount = 0;
    std::uint32_t sourceLocationIndex = 0;
};

struct CompiledBusIdSetV1 {
    std::vector<harkenstudio::ObjectId> ids;
};

struct CompiledBusResultV1 {
    std::uint64_t projectGeneration = 0;
    std::vector<CompiledBusV1> buses;
    std::vector<CompiledBusSendV1> sends;
    CompiledBusIdSetV1 idSet;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1> identityIndex;
    std::shared_ptr<const CompileTargetProfileV1> compileTargetProfile;
    std::uint64_t compileTargetProfileFingerprint = 0;
    std::vector<CompiledBusEffectV1> effects;
    std::vector<CompiledBusEffectParameterV1> effectParameters;
    std::vector<CompiledBusDownmixV1> downmix;
    std::vector<float> downmixCoefficients;
    std::uint64_t authoredBusFingerprint = 0;
    std::uint64_t checkedByteSize = 0;
    // Source locations are retained in first-reference order for diagnostics
    // and reproducible tooling proof bytes. They never enter the semantic
    // Runtime/Pack fingerprint stream.
    std::vector<BusSourceLocationV1> sourceLocations;
    std::vector<std::uint8_t> deterministicBytes;
};

enum class BusPublicationCapabilityVintageV1 : std::uint32_t {
    TopologyAndSendsNeutralProperties = 1,
    FullAuthoredProperties = 2,
    TopologyAndVolumeNeutralOtherProperties = 3,
    TopologyVolumeAndSendNeutralOtherProperties = 4,
};

struct PublishedBusProjectionV1 {
    BusPublicationCapabilityVintageV1 capabilityVintage =
        BusPublicationCapabilityVintageV1::TopologyAndSendsNeutralProperties;
    std::uint64_t projectGeneration = 0;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1> identityIndex;
    std::shared_ptr<const CompileTargetProfileV1> compileTargetProfile;
    std::uint64_t compileTargetProfileFingerprint = 0;
    std::vector<CompiledBusV1> buses;
    std::vector<CompiledBusSendV1> sends;
    std::vector<CompiledBusEffectV1> effects;
    std::vector<CompiledBusEffectParameterV1> effectParameters;
    std::vector<CompiledBusDownmixV1> downmix;
    std::vector<float> downmixCoefficients;
    std::uint64_t fingerprint = 0;
};

struct BusCompilerInputV1 {
    std::uint64_t projectGeneration = 0;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1> identityIndex;
    std::shared_ptr<const CompileTargetProfileV1> compileTargetProfile;
    std::vector<BusAuthoringV1> buses;
    std::vector<BusSendAuthoringV1> sends;
    std::vector<BusEffectAuthoringV1> effects;
    std::vector<BusEffectParameterAuthoringV1> effectParameters;
    std::vector<BusDownmixAuthoringV1> downmix;
    std::vector<float> downmixCoefficients;
    BusCompilerCapacityV1 capacity;
};

struct BusCompileOutcomeV1 {
    std::shared_ptr<const CompiledBusResultV1> result;
    std::vector<BusDiagnosticV1> diagnostics;

    bool ok() const noexcept { return result != nullptr && diagnostics.empty(); }
};

BusCompileOutcomeV1 CompileBusProjectV1(const BusCompilerInputV1& input);
std::vector<std::uint8_t> SerializeCompiledBusResultV1(const CompiledBusResultV1& result);
PublishedBusProjectionV1 BuildPublishedBusProjectionV1(
    const CompiledBusResultV1& result,
    BusPublicationCapabilityVintageV1 capabilityVintage);

} // namespace harkenbuscompiler
