#include "harken_bus_compiler/BusCompiler.h"

#include <harken_studio_build/ChannelLayout.h>
#include <harken_studio_build/BusFingerprint.h>
#include <harken_studio_build/EffectEncoding.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <iterator>
#include <queue>
#include <set>
#include <string_view>
#include <tuple>

namespace harkenbuscompiler {
namespace {

constexpr std::string_view kAuthoredBusFingerprintTagV1 = "harken.bus.authored.v1";
constexpr std::string_view kPublishedBusFingerprintTagV1 = "harken.bus.published.v1";
constexpr std::string_view kPublishedBusFingerprintTagV2 = "harken.bus.published.v2";

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    harkenstudiobuild::busfingerprint::AppendU32(
        [&bytes](std::uint8_t byte) { bytes.push_back(byte); }, value);
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    harkenstudiobuild::busfingerprint::AppendU64(
        [&bytes](std::uint8_t byte) { bytes.push_back(byte); }, value);
}

void appendF32(std::vector<std::uint8_t>& bytes, float value)
{
    appendU32(bytes, harkenstudiobuild::busfingerprint::CanonicalFloatBits(value));
}

void align8(std::vector<std::uint8_t>& bytes)
{
    while ((bytes.size() & 7u) != 0)
        bytes.push_back(0);
}

std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes)
{
    return harkenstudiobuild::busfingerprint::HashBytes(bytes.data(), bytes.size());
}

bool checkedAdd(std::uint64_t left, std::uint64_t right,
                std::uint64_t& output)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    output = left + right;
    return true;
}

bool checkedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t& output)
{
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    output = left * right;
    return true;
}

std::uint32_t channelCount(harkenstudio::ChannelLayout layout)
{
    switch (layout) {
    case harkenstudio::kChannelLayoutMono: return 1;
    case harkenstudio::kChannelLayoutStereo: return 2;
    case harkenstudio::kChannelLayoutQuad: return 4;
    case harkenstudio::kChannelLayout5_1: return 6;
    case harkenstudio::kChannelLayout7_1: return 8;
    default: return 0;
    }
}

enum class TargetProfileValidity {
    Valid,
    Malformed,
    UnsupportedLayout,
    CapabilityUnavailable,
};

TargetProfileValidity classifyTargetProfile(
    const CompileTargetProfileV1* profile)
{
    if (profile == nullptr)
        return TargetProfileValidity::Malformed;
    const auto mixChannels = channelCount(profile->mixLayout);
    const auto deviceChannels = channelCount(profile->deviceLayout);
    const bool fingerprintValid = profile->fingerprint != 0
        && harkenprojectschema::compileTargetProfileFingerprintV1(*profile)
            == profile->fingerprint;
    if (profile->vintage != 1 || profile->sampleRate < 1
        || profile->sampleRate > 384000
        || (profile->capabilityMask
            & ~harkenprojectschema::kKnownCapabilityMaskV1) != 0) {
        return TargetProfileValidity::Malformed;
    }
    if (mixChannels == 0 || deviceChannels == 0
        || deviceChannels > mixChannels) {
        return TargetProfileValidity::UnsupportedLayout;
    }
    if (!fingerprintValid)
        return TargetProfileValidity::Malformed;
    if ((profile->capabilityMask & kBusRequiredCapabilityMaskV1)
        != kBusRequiredCapabilityMaskV1) {
        return TargetProfileValidity::CapabilityUnavailable;
    }
    return TargetProfileValidity::Valid;
}

bool validTargetEffectParameter(const CompileTargetProfileV1& profile,
                                std::uint32_t effectType,
                                std::uint32_t parameterIndex,
                                float value)
{
    if (!harkenstudiobuild::PackEffectParamInAuthoredRange(
            effectType, parameterIndex, value))
        return false;
    if (effectType == harkenstudio::kEffectThreeBandTone
        && (parameterIndex == 1 || parameterIndex == 3
            || parameterIndex == 6)) {
        return value < static_cast<float>(profile.sampleRate) * 0.5f;
    }
    return true;
}

bool hasIdentity(const std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>& index,
                 harkenstudio::ObjectId id, harkenprojectschema::RuntimeIdentityKindV1 kind)
{
    if (!index)
        return false;
    const auto* claim = index->find(id);
    return claim != nullptr && claim->kind == kind;
}

BusDiagnosticV1 diagnostic(BusDiagnosticCodeV1 code, harkenstudio::ObjectId offending,
                           harkenstudio::ObjectId related, std::uint64_t generation,
                           const BusSourceLocationV1& source = {})
{
    return {code, offending, related, generation, source};
}

BusDiagnosticV1 numericDiagnostic(BusDiagnosticCodeV1 code, std::uint64_t actual,
                                  std::uint64_t limit, std::uint64_t generation)
{
    return {code, {}, {}, generation, {}, actual, limit};
}

BusDiagnosticV1 detailDiagnostic(BusDiagnosticCodeV1 code, harkenstudio::ObjectId object,
                                 std::uint64_t detail, std::uint64_t generation,
                                 const BusSourceLocationV1& source)
{
    return {code, object, {}, generation, source, detail};
}

void appendObjectId(std::vector<std::uint8_t>& out, harkenstudio::ObjectId value)
{
    const auto bytes = harkenstudio::ObjectIdBytes(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendProfile(std::vector<std::uint8_t>& bytes,
                   const CompileTargetProfileV1* profile)
{
    if (profile == nullptr) {
        appendU32(bytes, 0);
        appendU32(bytes, 0);
        appendU32(bytes, 0);
        appendU32(bytes, 0);
        appendU64(bytes, 0);
        return;
    }
    appendU32(bytes, profile->vintage);
    appendU32(bytes, profile->sampleRate);
    appendU32(bytes, profile->mixLayout);
    appendU32(bytes, profile->deviceLayout);
    appendU64(bytes, profile->capabilityMask);
}

void appendBusRow(std::vector<std::uint8_t>& bytes,
                  const CompiledBusV1& bus)
{
    appendObjectId(bytes, bus.busId);
    appendObjectId(bytes, bus.parentId);
    appendU32(bytes, bus.feedsFirstOrdinal);
    appendF32(bytes, bus.faderDb);
    appendU32(bytes, bus.faderAfterSlot);
    appendU32(bytes, bus.outputLayout);
    appendU32(bytes, bus.effectFirst);
    appendU32(bytes, bus.effectCount);
    appendU32(bytes, bus.downmixFirst);
    appendU32(bytes, bus.downmixCount);
}

void appendEffectRow(std::vector<std::uint8_t>& bytes,
                     const CompiledBusEffectV1& effect)
{
    appendObjectId(bytes, effect.busId);
    appendObjectId(bytes, effect.effectId);
    appendU32(bytes, effect.slot);
    appendU32(bytes, effect.effectType);
    appendU32(bytes, effect.descriptorVintage);
    appendU32(bytes, effect.flags);
    appendU32(bytes, effect.parameterFirst);
    appendU32(bytes, effect.parameterCount);
}

bool sameSource(const BusSourceLocationV1& left,
                const BusSourceLocationV1& right)
{
    return left.authoringGuid == right.authoringGuid
        && left.documentPath == right.documentPath
        && left.fieldPath == right.fieldPath;
}

bool validSourceLocation(const BusSourceLocationV1& source)
{
    if (harkenprojectschema::IsNull(source.authoringGuid)
        || source.documentPath.empty() || source.fieldPath.empty()
        || source.documentPath.front() == '/'
        || source.documentPath.find('\\') != std::string::npos
        || (source.documentPath.size() >= 2
            && source.documentPath[1] == ':'))
        return false;
    std::size_t segmentStart = 0;
    while (segmentStart <= source.documentPath.size()) {
        const std::size_t separator = source.documentPath.find(
            '/', segmentStart);
        const std::size_t segmentEnd = separator == std::string::npos
            ? source.documentPath.size() : separator;
        const std::string_view segment(
            source.documentPath.data() + segmentStart,
            segmentEnd - segmentStart);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (separator == std::string::npos) break;
        segmentStart = separator + 1;
    }
    return true;
}

std::uint32_t appendSource(std::vector<BusSourceLocationV1>& locations,
                           const BusSourceLocationV1& source)
{
    const auto found = std::find_if(locations.begin(), locations.end(),
                                    [&](const auto& existing) {
                                        return sameSource(existing, source);
                                    });
    if (found == locations.end()) {
        locations.push_back(source);
        return static_cast<std::uint32_t>(locations.size() - 1);
    }
    return static_cast<std::uint32_t>(
        std::distance(locations.begin(), found));
}

bool checkedAlign8(std::uint64_t& value)
{
    std::uint64_t aligned = 0;
    if (!checkedAdd(value, 7, aligned)) return false;
    value = aligned & ~UINT64_C(7);
    return true;
}

bool checkedSection(std::uint64_t count, std::uint64_t stride,
                    std::uint64_t& size)
{
    std::uint64_t bytes = 0;
    return checkedMultiply(count, stride, bytes)
        && checkedAdd(size, bytes, size)
        && checkedAlign8(size);
}

bool serializedProofSize(const CompiledBusResultV1& result,
                         std::uint64_t& size,
                         std::uint32_t& sourceDataBytes)
{
    if (result.buses.size() > UINT32_MAX
        || result.sends.size() > UINT32_MAX
        || result.effects.size() > UINT32_MAX
        || result.effectParameters.size() > UINT32_MAX
        || result.downmix.size() > UINT32_MAX
        || result.downmixCoefficients.size() > UINT32_MAX
        || result.sourceLocations.size() > UINT32_MAX)
        return false;

    const auto sourceIndexValid = [&](std::uint32_t index) {
        return index < result.sourceLocations.size();
    };
    if (std::any_of(result.buses.begin(), result.buses.end(),
                    [&](const auto& row) {
                        return !sourceIndexValid(row.sourceLocationIndex);
                    })
        || std::any_of(result.sends.begin(), result.sends.end(),
                       [&](const auto& row) {
                           return !sourceIndexValid(row.sourceLocationIndex);
                       })
        || std::any_of(result.effects.begin(), result.effects.end(),
                       [&](const auto& row) {
                           return !sourceIndexValid(row.sourceLocationIndex);
                       })
        || std::any_of(result.effectParameters.begin(),
                       result.effectParameters.end(), [&](const auto& row) {
                           return !sourceIndexValid(row.sourceLocationIndex);
                       })
        || std::any_of(result.downmix.begin(), result.downmix.end(),
                       [&](const auto& row) {
                           return !sourceIndexValid(row.sourceLocationIndex);
                       }))
        return false;

    std::uint64_t sourceBytes = 0;
    for (const auto& source : result.sourceLocations) {
        if (source.documentPath.size() > UINT32_MAX
            || source.fieldPath.size() > UINT32_MAX
            || !checkedAdd(sourceBytes, source.documentPath.size(), sourceBytes)
            || !checkedAdd(sourceBytes, source.fieldPath.size(), sourceBytes)
            || sourceBytes > UINT32_MAX)
            return false;
    }

    size = 96;
    if (!checkedSection(result.buses.size(), 96, size)
        || !checkedSection(result.sends.size(), 48, size)
        || !checkedSection(result.effects.size(), 64, size)
        || !checkedSection(result.effectParameters.size(), 16, size)
        || !checkedSection(result.downmix.size(), 48, size)
        || !checkedSection(result.downmixCoefficients.size(), 4, size)
        || !checkedSection(result.sourceLocations.size(), 40, size)
        || !checkedAdd(size, sourceBytes, size))
        return false;
    sourceDataBytes = static_cast<std::uint32_t>(sourceBytes);
    return size <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max());
}

std::uint32_t resolvedOutputLayout(const BusAuthoringV1& bus,
                                   const CompileTargetProfileV1& profile)
{
    if (bus.outputLayout == harkenstudio::kChannelLayoutDefault)
        return profile.mixLayout;
    const auto count = channelCount(bus.outputLayout);
    const auto mixCount = channelCount(profile.mixLayout);
    if (count == 0 || count > mixCount)
        return 0;
    return bus.outputLayout;
}

void addDefaultMatrix(std::vector<float>& destination,
                      std::uint32_t sourceLayout,
                      std::uint32_t destinationLayout)
{
    float matrix[64] = {};
    harkenstudiobuild::DefaultDownmixMatrix(sourceLayout, destinationLayout, matrix);
    destination.insert(destination.end(), std::begin(matrix), std::end(matrix));
}

bool checkedFingerprintSection(std::uint64_t count, std::uint64_t stride,
                               std::uint64_t& size)
{
    std::uint64_t bytes = 0;
    return checkedMultiply(count, stride, bytes)
        && checkedAdd(size, bytes, size);
}

template <typename Result>
bool checkedFingerprintStreamSize(const Result& result, bool published,
                                  std::uint64_t& size,
                                  BusPublicationCapabilityVintageV1 capabilityVintage =
                                      BusPublicationCapabilityVintageV1::TopologyAndSendsNeutralProperties)
{
    if (result.buses.size() > UINT32_MAX
        || result.sends.size() > UINT32_MAX
        || result.effects.size() > UINT32_MAX
        || result.effectParameters.size() > UINT32_MAX
        || result.downmix.size() > UINT32_MAX
        || result.downmixCoefficients.size() > UINT32_MAX)
        return false;

    size = published ? 68 + kPublishedBusFingerprintTagV1.size()
                     : 64 + kAuthoredBusFingerprintTagV1.size();
    const bool volumeOnly = published && (capabilityVintage ==
        BusPublicationCapabilityVintageV1::TopologyAndVolumeNeutralOtherProperties ||
        capabilityVintage == BusPublicationCapabilityVintageV1::TopologyVolumeAndSendNeutralOtherProperties);
    return checkedFingerprintSection(result.buses.size(), volumeOnly ? 72 : 64, size)
        && checkedFingerprintSection(result.sends.size(), 40, size)
        && checkedFingerprintSection(result.effects.size(), 56, size)
        && checkedFingerprintSection(result.effectParameters.size(), 8, size)
        && checkedFingerprintSection(result.downmix.size(), 32, size)
        && checkedFingerprintSection(
            result.downmixCoefficients.size(), 4, size)
        && size <= static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max());
}

template <typename Result>
bool fingerprintStream(
    const Result& result, bool published,
    BusPublicationCapabilityVintageV1 capabilityVintage,
    std::vector<std::uint8_t>& bytes)
{
    std::uint64_t checkedSize = 0;
    if (!checkedFingerprintStreamSize(result, published, checkedSize,
                                      capabilityVintage))
        return false;
    bytes.clear();
    bytes.reserve(static_cast<std::size_t>(checkedSize));
    const bool volumeOnly = published && (capabilityVintage ==
        BusPublicationCapabilityVintageV1::TopologyAndVolumeNeutralOtherProperties ||
        capabilityVintage == BusPublicationCapabilityVintageV1::TopologyVolumeAndSendNeutralOtherProperties);
    const std::string_view tag = volumeOnly ? kPublishedBusFingerprintTagV2
        : published ? kPublishedBusFingerprintTagV1 : kAuthoredBusFingerprintTagV1;
    appendU32(bytes, static_cast<std::uint32_t>(tag.size()));
    bytes.insert(bytes.end(), tag.begin(), tag.end());
    appendU32(bytes, volumeOnly ? 2 : 1);
    if (published)
        appendU32(bytes, static_cast<std::uint32_t>(capabilityVintage));
    appendProfile(bytes, result.compileTargetProfile.get());
    const std::uint64_t profileFingerprint = result.compileTargetProfile
        ? (result.compileTargetProfileFingerprint != 0
               ? result.compileTargetProfileFingerprint
               : harkenprojectschema::compileTargetProfileFingerprintV1(
                     *result.compileTargetProfile))
        : 0;
    appendU64(bytes, profileFingerprint);
    appendU32(bytes, static_cast<std::uint32_t>(result.buses.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.sends.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.effects.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.effectParameters.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.downmix.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.downmixCoefficients.size()));
    for (const auto& bus : result.buses) {
        appendBusRow(bytes, bus);
        if (volumeOnly) {
            appendF32(bytes, bus.makeUpGainDb);
            appendF32(bytes, bus.outputBusVolumeDb);
        }
    }
    for (const auto& send : result.sends) {
        appendObjectId(bytes, send.sourceBusId);
        appendObjectId(bytes, send.targetBusId);
        appendF32(bytes, send.sendLevelDb);
        appendU32(bytes, 0);
    }
    for (const auto& effect : result.effects)
        appendEffectRow(bytes, effect);
    for (const auto& parameter : result.effectParameters) {
        appendU32(bytes, parameter.index);
        appendF32(bytes, parameter.value);
    }
    for (const auto& row : result.downmix) {
        appendObjectId(bytes, row.busId);
        appendU32(bytes, row.sourceLayout);
        appendU32(bytes, row.destinationLayout);
        appendU32(bytes, row.coefficientFirst);
        appendU32(bytes, row.coefficientCount);
    }
    for (const float coefficient : result.downmixCoefficients)
        appendF32(bytes, coefficient);
    return bytes.size() == checkedSize;
}

} // namespace

std::vector<std::uint8_t> SerializeCompiledBusResultV1(
    const CompiledBusResultV1& result)
{
    std::uint64_t proofSize = 0;
    std::uint32_t sourceDataBytes = 0;
    if (!serializedProofSize(result, proofSize, sourceDataBytes))
        return {};

    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(proofSize));
    appendU32(bytes, 1);
    appendU32(bytes, 96);
    appendU64(bytes, result.projectGeneration);
    appendProfile(bytes, result.compileTargetProfile.get());
    appendU64(bytes, result.compileTargetProfileFingerprint);
    appendU32(bytes, static_cast<std::uint32_t>(result.buses.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.sends.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.effects.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.effectParameters.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.downmix.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.downmixCoefficients.size()));
    appendU32(bytes, static_cast<std::uint32_t>(result.sourceLocations.size()));
    appendU32(bytes, 0);
    appendU64(bytes, result.authoredBusFingerprint);
    appendU64(bytes, proofSize);
    align8(bytes);
    for (const auto& bus : result.buses) {
        appendBusRow(bytes, bus);
        appendU32(bytes, bus.sourceLocationIndex);
        for (unsigned index = 0; index < 7; ++index)
            appendU32(bytes, 0);
    }
    align8(bytes);
    for (const auto& send : result.sends) {
        appendObjectId(bytes, send.sourceBusId);
        appendObjectId(bytes, send.targetBusId);
        appendF32(bytes, send.sendLevelDb);
        appendU32(bytes, 0);
        appendU32(bytes, send.sourceLocationIndex);
        appendU32(bytes, 0);
    }
    align8(bytes);
    for (const auto& effect : result.effects) {
        appendEffectRow(bytes, effect);
        appendU32(bytes, effect.sourceLocationIndex);
        appendU32(bytes, 0);
    }
    align8(bytes);
    for (const auto& parameter : result.effectParameters) {
        appendU32(bytes, parameter.index);
        appendF32(bytes, parameter.value);
        appendU32(bytes, parameter.sourceLocationIndex);
        appendU32(bytes, 0);
    }
    align8(bytes);
    for (const auto& row : result.downmix) {
        appendObjectId(bytes, row.busId);
        appendU32(bytes, row.sourceLayout);
        appendU32(bytes, row.destinationLayout);
        appendU32(bytes, row.coefficientFirst);
        appendU32(bytes, row.coefficientCount);
        appendU32(bytes, row.sourceLocationIndex);
        appendU32(bytes, 0);
        appendU32(bytes, 0);
        appendU32(bytes, 0);
    }
    align8(bytes);
    for (const float coefficient : result.downmixCoefficients)
        appendF32(bytes, coefficient);
    align8(bytes);

    std::uint32_t sourceOffset = 0;
    for (const auto& source : result.sourceLocations) {
        appendU64(bytes, source.authoringGuid.high);
        appendU64(bytes, source.authoringGuid.low);
        appendU32(bytes, sourceOffset);
        appendU32(bytes, static_cast<std::uint32_t>(source.documentPath.size()));
        sourceOffset += static_cast<std::uint32_t>(source.documentPath.size());
        appendU32(bytes, sourceOffset);
        appendU32(bytes, static_cast<std::uint32_t>(source.fieldPath.size()));
        sourceOffset += static_cast<std::uint32_t>(source.fieldPath.size());
        appendU64(bytes, 0);
    }
    align8(bytes);
    for (const auto& source : result.sourceLocations) {
        bytes.insert(bytes.end(), source.documentPath.begin(),
                     source.documentPath.end());
        bytes.insert(bytes.end(), source.fieldPath.begin(),
                     source.fieldPath.end());
    }
    if (sourceOffset != sourceDataBytes || bytes.size() != proofSize)
        return {};
    return bytes;
}

BusCompileOutcomeV1 CompileBusProjectV1(const BusCompilerInputV1& input)
{
    BusCompileOutcomeV1 outcome;
    constexpr std::uint64_t kHardMaximumCompiledBytes =
        UINT64_C(256) * 1024 * 1024;
    if (!input.identityIndex) {
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::MissingIdentity, {}, {},
            input.projectGeneration));
    }
    const TargetProfileValidity profileValidity = classifyTargetProfile(
        input.compileTargetProfile.get());
    if (profileValidity == TargetProfileValidity::Malformed) {
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::UnsupportedAuthoredProperty, {}, {},
            input.projectGeneration));
    } else if (profileValidity == TargetProfileValidity::UnsupportedLayout) {
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::InvalidLayoutDownmix, {}, {},
            input.projectGeneration));
    } else if (profileValidity
               == TargetProfileValidity::CapabilityUnavailable) {
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::TargetCapabilityUnavailable, {}, {},
            input.projectGeneration));
    }
    if (input.buses.size() >= input.capacity.maxBusesIncludingOutputRoot) {
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::BusCapacity,
            static_cast<std::uint64_t>(input.buses.size()) + 1,
            input.capacity.maxBusesIncludingOutputRoot,
            input.projectGeneration));
    }
    if (input.capacity.maxCompiledBytes == 0
        || input.capacity.maxCompiledBytes > kHardMaximumCompiledBytes) {
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::CompiledByteCapacity,
            input.capacity.maxCompiledBytes, kHardMaximumCompiledBytes,
            input.projectGeneration));
    }
    if (input.buses.size() > UINT32_MAX || input.sends.size() > UINT32_MAX
        || input.effects.size() > UINT32_MAX
        || input.effectParameters.size() > UINT32_MAX
        || input.downmix.size() > UINT32_MAX
        || input.downmixCoefficients.size() > UINT32_MAX) {
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::CompiledByteCapacity, {}, {},
            input.projectGeneration));
    }

    std::map<harkenstudio::ObjectId, BusAuthoringV1> byId;
    std::size_t masterCount = 0;
    harkenstudio::ObjectId masterId{};
    for (const auto& bus : input.buses) {
        if (!validSourceLocation(bus.source)) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::UnsupportedAuthoredProperty,
                bus.busId, {}, input.projectGeneration, bus.source));
        }
        if (bus.busId == harkenstudio::ObjectId{}) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::MissingIdentity, bus.busId, bus.parentId,
                input.projectGeneration, bus.source));
            continue;
        }
        if (input.identityIndex
            && !hasIdentity(input.identityIndex, bus.busId,
                            harkenprojectschema::RuntimeIdentityKindV1::Bus)) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::MissingIdentity, bus.busId, {},
                input.projectGeneration, bus.source));
        }
        if (bus.parentId == harkenstudio::kMasterBusObjectId) {
            ++masterCount;
            masterId = bus.busId;
        }
        if (bus.busId == harkenstudio::kMasterBusObjectId)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::ReservedMasterCollision, bus.busId,
                bus.parentId, input.projectGeneration, bus.source));
        if (!byId.emplace(bus.busId, bus).second)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::DuplicateIdentity, bus.busId, {},
                input.projectGeneration, bus.source));
        if (!std::isfinite(bus.faderDb)
            || bus.faderDb < -96.0f || bus.faderDb > 24.0f)
            outcome.diagnostics.push_back(diagnostic(
                std::isfinite(bus.faderDb)
                    ? BusDiagnosticCodeV1::UnsupportedAuthoredProperty
                    : BusDiagnosticCodeV1::NonFiniteScalar,
                bus.busId, {},
                input.projectGeneration, bus.source));
        const auto convertible = [](float db) {
            return std::isfinite(db) &&
                std::isfinite(std::pow(10.0f, db / 20.0f));
        };
        if (!convertible(bus.makeUpGainDb) ||
            !convertible(bus.outputBusVolumeDb) ||
            !convertible(bus.faderDb + bus.makeUpGainDb) ||
            !convertible(bus.faderDb + bus.makeUpGainDb + bus.outputBusVolumeDb))
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::NonFiniteScalar, bus.busId, {},
                input.projectGeneration, bus.source));
        if (bus.faderAfterSlot > input.capacity.maxEffectsPerBus)
            outcome.diagnostics.push_back(detailDiagnostic(
                BusDiagnosticCodeV1::InvalidFaderPoint, bus.busId,
                bus.faderAfterSlot, input.projectGeneration, bus.source));
        if (bus.effectFirst > input.effects.size()
            || bus.effectCount > input.effects.size() - bus.effectFirst)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::EffectCapacity, bus.busId, {},
                input.projectGeneration, bus.source));
        if (bus.downmixFirst > input.downmix.size()
            || bus.downmixCount > input.downmix.size() - bus.downmixFirst)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::InvalidLayoutDownmix, bus.busId, {},
                input.projectGeneration, bus.source));
    }
    if (masterCount == 0)
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::MissingMaster, harkenstudio::kMasterBusObjectId, {},
            input.projectGeneration));
    else if (masterCount > 1)
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::MultipleMasterCandidates, harkenstudio::kMasterBusObjectId, {},
            input.projectGeneration));

    for (const auto& [id, bus] : byId) {
        if (bus.parentId == id) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::SelfEdge, id, bus.parentId,
                input.projectGeneration, bus.source));
        }
        if (bus.parentId != harkenstudio::kMasterBusObjectId
            && (bus.parentId == harkenstudio::ObjectId{} || !byId.count(bus.parentId))) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::MissingParent, id, bus.parentId,
                input.projectGeneration, bus.source));
        }
    }
    if (byId.size() >= input.capacity.maxBusesIncludingOutputRoot)
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::BusCapacity,
            static_cast<std::uint64_t>(byId.size()) + 1,
            input.capacity.maxBusesIncludingOutputRoot, input.projectGeneration));

    std::map<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>, BusSendAuthoringV1> sendRows;
    std::map<harkenstudio::ObjectId, std::uint32_t> sendsPerBus;
    if (input.sends.size() > input.capacity.maxTotalSends)
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::TotalSendCapacity,
            static_cast<std::uint64_t>(input.sends.size()), 0,
            input.projectGeneration));
    for (const auto& send : input.sends) {
        if (!validSourceLocation(send.source)) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::UnsupportedAuthoredProperty,
                send.sourceBusId, send.targetBusId,
                input.projectGeneration, send.source));
        }
        const bool sourceExists = byId.count(send.sourceBusId) != 0;
        const bool targetExists = byId.count(send.targetBusId) != 0;
        if (!sourceExists || !targetExists)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::MissingSendTarget, send.sourceBusId,
                send.targetBusId, input.projectGeneration, send.source));
        if (sourceExists && input.identityIndex
            && !hasIdentity(input.identityIndex, send.sourceBusId,
                            harkenprojectschema::RuntimeIdentityKindV1::Bus))
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::MissingIdentity, send.sourceBusId,
                send.targetBusId, input.projectGeneration, send.source));
        if (targetExists && input.identityIndex
            && !hasIdentity(input.identityIndex, send.targetBusId,
                            harkenprojectschema::RuntimeIdentityKindV1::Bus))
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::MissingIdentity, send.targetBusId,
                send.sourceBusId, input.projectGeneration, send.source));
        if (send.sourceBusId == send.targetBusId)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::SelfEdge, send.sourceBusId,
                send.targetBusId, input.projectGeneration, send.source));
        if (!std::isfinite(send.sendLevelDb)
            || send.sendLevelDb < -96.0f || send.sendLevelDb > 24.0f)
            outcome.diagnostics.push_back(diagnostic(
                std::isfinite(send.sendLevelDb)
                    ? BusDiagnosticCodeV1::UnsupportedAuthoredProperty
                    : BusDiagnosticCodeV1::NonFiniteScalar,
                send.sourceBusId, send.targetBusId,
                input.projectGeneration, send.source));
        if (sourceExists && targetExists
            && !sendRows.emplace(std::make_pair(send.sourceBusId,
                                                send.targetBusId), send).second)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::DuplicateIdentity, send.sourceBusId,
                send.targetBusId, input.projectGeneration, send.source));
        if (sourceExists && targetExists
            && ++sendsPerBus[send.sourceBusId] > input.capacity.maxSendsPerBus)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::PerBusSendCapacity, send.sourceBusId, {},
                input.projectGeneration, send.source));
    }
    if (sendRows.size() > input.capacity.maxTotalSends)
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::TotalSendCapacity,
            static_cast<std::uint64_t>(sendRows.size()), 0,
            input.projectGeneration));

    std::vector<std::uint32_t> effectSliceOwners(input.effects.size(), 0);
    std::vector<std::uint32_t> downmixSliceOwners(input.downmix.size(), 0);
    for (const auto& bus : input.buses) {
        if (bus.effectFirst <= input.effects.size()
            && bus.effectCount <= input.effects.size() - bus.effectFirst) {
            for (std::uint32_t offset = 0; offset < bus.effectCount; ++offset) {
                const std::size_t position = bus.effectFirst + offset;
                ++effectSliceOwners[position];
                if (input.effects[position].busId != bus.busId)
                    outcome.diagnostics.push_back(diagnostic(
                        BusDiagnosticCodeV1::EffectCapacity, bus.busId,
                        input.effects[position].effectId,
                        input.projectGeneration, bus.source));
            }
        }
        if (bus.downmixFirst <= input.downmix.size()
            && bus.downmixCount <= input.downmix.size() - bus.downmixFirst) {
            for (std::uint32_t offset = 0; offset < bus.downmixCount; ++offset) {
                const std::size_t position = bus.downmixFirst + offset;
                ++downmixSliceOwners[position];
                if (input.downmix[position].busId != bus.busId)
                    outcome.diagnostics.push_back(diagnostic(
                        BusDiagnosticCodeV1::InvalidLayoutDownmix, bus.busId,
                        input.downmix[position].busId,
                        input.projectGeneration, bus.source));
            }
        }
    }
    if (std::any_of(effectSliceOwners.begin(), effectSliceOwners.end(),
                    [](std::uint32_t owners) { return owners != 1; }))
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::EffectCapacity, {}, {},
            input.projectGeneration));
    if (std::any_of(downmixSliceOwners.begin(), downmixSliceOwners.end(),
                    [](std::uint32_t owners) { return owners != 1; }))
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::InvalidLayoutDownmix, {}, {},
            input.projectGeneration));

    std::map<harkenstudio::ObjectId, std::set<std::uint32_t>> effectSlots;
    std::map<std::pair<harkenstudio::ObjectId, std::uint32_t>, BusEffectAuthoringV1> effectRows;
    std::map<harkenstudio::ObjectId, harkenstudio::ObjectId> effectIds;
    std::vector<std::uint32_t> parameterSliceOwners(
        input.effectParameters.size(), 0);
    for (const auto& effect : input.effects) {
        if (!validSourceLocation(effect.source)) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::UnsupportedAuthoredProperty,
                effect.effectId, effect.busId,
                input.projectGeneration, effect.source));
        }
        if (!byId.count(effect.busId) || effect.effectId == harkenstudio::ObjectId{}) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::MissingIdentity, effect.effectId,
                effect.busId, input.projectGeneration, effect.source));
            continue;
        }
        if (input.identityIndex
            && !hasIdentity(input.identityIndex, effect.effectId,
                            harkenprojectschema::RuntimeIdentityKindV1::BusEffect))
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::MissingIdentity, effect.effectId,
                effect.busId, input.projectGeneration, effect.source));
        if (!effectIds.emplace(effect.effectId, effect.busId).second)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::DuplicateIdentity, effect.effectId,
                effect.busId, input.projectGeneration, effect.source));
        if (!effectSlots[effect.busId].insert(effect.slot).second)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::EffectCapacity, effect.busId,
                effect.effectId, input.projectGeneration, effect.source));
        if (effect.slot >= input.capacity.maxEffectsPerBus)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::EffectCapacity, effect.busId,
                effect.effectId, input.projectGeneration, effect.source));
        const auto* effectSchema = harkenstudiobuild::FindPackEffectSchemaByType(
            effect.effectType);
        if (!effectSchema
            || effect.descriptorVintage != effectSchema->descriptorVintage)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::UnsupportedAuthoredProperty, effect.busId,
                effect.effectId, input.projectGeneration, effect.source));
        if (!effectSchema)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::TargetCapabilityUnavailable, effect.busId,
                effect.effectId, input.projectGeneration, effect.source));
        if (input.compileTargetProfile) {
            const std::uint64_t effectTypeCapability = effect.effectType <= 4
                ? (UINT64_C(1) << (32 + effect.effectType)) : 0;
            if ((input.compileTargetProfile->capabilityMask
                 & (UINT64_C(1) << 19)) == 0
                || effectTypeCapability == 0
                || (input.compileTargetProfile->capabilityMask
                    & effectTypeCapability) == 0)
                outcome.diagnostics.push_back(diagnostic(
                    BusDiagnosticCodeV1::TargetCapabilityUnavailable,
                    effect.busId, effect.effectId, input.projectGeneration,
                    effect.source));
        }
        if (effect.parameterFirst > input.effectParameters.size()
            || effect.parameterCount > input.effectParameters.size()
                - effect.parameterFirst)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::UnsupportedAuthoredProperty, effect.busId,
                effect.effectId, input.projectGeneration, effect.source));
        else {
            if (!effectSchema
                || effect.parameterCount != effectSchema->paramCount)
                outcome.diagnostics.push_back(diagnostic(
                    BusDiagnosticCodeV1::UnsupportedAuthoredProperty,
                    effect.busId, effect.effectId,
                    input.projectGeneration, effect.source));
            for (std::uint32_t index = 0;
                 index < effect.parameterCount; ++index) {
                const std::size_t position = effect.parameterFirst + index;
                ++parameterSliceOwners[position];
                const auto& parameter = input.effectParameters[position];
                if (!validSourceLocation(parameter.source))
                    outcome.diagnostics.push_back(detailDiagnostic(
                        BusDiagnosticCodeV1::UnsupportedAuthoredProperty,
                        parameter.effectId, parameter.index,
                        input.projectGeneration, parameter.source));
                if (parameter.effectId != effect.effectId
                    || parameter.index != index)
                    outcome.diagnostics.push_back(diagnostic(
                        BusDiagnosticCodeV1::UnsupportedAuthoredProperty,
                        effect.busId, effect.effectId,
                        input.projectGeneration, parameter.source));
                if (!std::isfinite(parameter.value))
                    outcome.diagnostics.push_back(detailDiagnostic(
                        BusDiagnosticCodeV1::NonFiniteScalar,
                        parameter.effectId, parameter.index,
                        input.projectGeneration, parameter.source));
                else if (effectSchema && input.compileTargetProfile
                         && !validTargetEffectParameter(
                             *input.compileTargetProfile, effect.effectType,
                             parameter.index, parameter.value))
                    outcome.diagnostics.push_back(detailDiagnostic(
                        BusDiagnosticCodeV1::UnsupportedAuthoredProperty,
                        parameter.effectId, parameter.index,
                        input.projectGeneration, parameter.source));
            }
        }
        effectRows.emplace(std::make_pair(effect.busId, effect.slot), effect);
    }
    std::uint64_t maxEffectsTotal = 0;
    if (!checkedMultiply(input.capacity.maxEffectsPerBus, byId.size(),
                         maxEffectsTotal)
        || input.effects.size() > maxEffectsTotal)
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::EffectCapacity,
            static_cast<std::uint64_t>(input.effects.size()), 0,
            input.projectGeneration));
    if (std::any_of(parameterSliceOwners.begin(),
                    parameterSliceOwners.end(),
                     [](std::uint32_t owners) { return owners != 1; }))
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::UnsupportedAuthoredProperty, {}, {},
            input.projectGeneration));

    if (input.downmix.size() > input.capacity.maxDownmixRows
        || input.downmixCoefficients.size()
            > input.capacity.maxDownmixCoefficients)
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::InvalidLayoutDownmix,
            static_cast<std::uint64_t>(input.downmix.size()), 0,
            input.projectGeneration));
    std::map<harkenstudio::ObjectId, BusDownmixAuthoringV1> downmixRows;
    for (const auto& row : input.downmix) {
        if (!validSourceLocation(row.source)) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::UnsupportedAuthoredProperty,
                row.busId, {}, input.projectGeneration, row.source));
        }
        if (!byId.count(row.busId)
            || row.coefficientFirst > input.downmixCoefficients.size()
            || row.coefficientCount > input.downmixCoefficients.size()
                - row.coefficientFirst
            || row.coefficientCount != 64)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::InvalidLayoutDownmix, row.busId, {},
                input.projectGeneration, row.source));
        if (!downmixRows.emplace(row.busId, row).second)
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::InvalidLayoutDownmix, row.busId, {},
                input.projectGeneration, row.source));
    }
    for (const float coefficient : input.downmixCoefficients)
        if (!std::isfinite(coefficient))
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::NonFiniteScalar, {}, {},
                input.projectGeneration));

    std::map<harkenstudio::ObjectId, std::vector<harkenstudio::ObjectId>> outgoing;
    std::map<harkenstudio::ObjectId, std::uint32_t> indegree;
    for (const auto& [id, unused] : byId)
        indegree.emplace(id, 0);
    const auto addEdge = [&](harkenstudio::ObjectId from, harkenstudio::ObjectId to) {
        if (!byId.count(from) || !byId.count(to))
            return;
        outgoing[from].push_back(to);
        ++indegree[to];
    };
    for (const auto& [id, bus] : byId)
        if (byId.count(bus.parentId))
            addEdge(id, bus.parentId);
    for (const auto& [key, unused] : sendRows)
        addEdge(key.first, key.second);

    std::priority_queue<harkenstudio::ObjectId, std::vector<harkenstudio::ObjectId>, std::greater<harkenstudio::ObjectId>> ready;
    for (const auto& [id, degree] : indegree)
        if (degree == 0)
            ready.push(id);
    std::vector<harkenstudio::ObjectId> order;
    while (!ready.empty()) {
        const harkenstudio::ObjectId id = ready.top();
        ready.pop();
        order.push_back(id);
        for (const harkenstudio::ObjectId target : outgoing[id])
            if (--indegree[target] == 0)
                ready.push(target);
    }
    if (order.size() != byId.size()) {
        const auto residual = std::find_if(indegree.begin(), indegree.end(),
                                           [](const auto& item) {
                                               return item.second != 0;
                                           });
        outcome.diagnostics.push_back(diagnostic(
            BusDiagnosticCodeV1::CombinedTreeSendCycle,
            residual == indegree.end() ? harkenstudio::ObjectId{} : residual->first, {},
            input.projectGeneration));
    }
    if (masterCount == 1) {
        const auto master = std::find(order.begin(), order.end(), masterId);
        if (master != order.end() && std::next(master) != order.end()) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::CombinedTreeSendCycle, masterId, {},
                input.projectGeneration));
        }
    }

    if (!outcome.diagnostics.empty()) {
        std::sort(outcome.diagnostics.begin(), outcome.diagnostics.end(),
                  [](const auto& left, const auto& right) {
                      return std::tie(left.code, left.offendingId,
                                      left.relatedId, left.source.documentPath,
                                      left.source.fieldPath)
                          < std::tie(right.code, right.offendingId,
                                      right.relatedId, right.source.documentPath,
                                      right.source.fieldPath);
                  });
        return outcome;
    }

    auto result = std::make_shared<CompiledBusResultV1>();
    result->projectGeneration = input.projectGeneration;
    result->identityIndex = input.identityIndex;
    result->compileTargetProfile = input.compileTargetProfile;
    result->compileTargetProfileFingerprint = input.compileTargetProfile
        ? (input.compileTargetProfile->fingerprint != 0
               ? input.compileTargetProfile->fingerprint
               : harkenprojectschema::compileTargetProfileFingerprintV1(
                     *input.compileTargetProfile))
        : 0;
    result->idSet.ids.reserve(byId.size());
    for (const auto& [id, unused] : byId)
        result->idSet.ids.push_back(id);

    for (std::uint32_t ordinal = 0; ordinal < order.size(); ++ordinal) {
        const auto& bus = byId.at(order[ordinal]);
        const auto resolved = resolvedOutputLayout(
            bus, *input.compileTargetProfile);
        if (resolved == 0) {
            outcome.diagnostics.push_back(diagnostic(
                BusDiagnosticCodeV1::InvalidLayoutDownmix, bus.busId, {},
                input.projectGeneration, bus.source));
            continue;
        }
        const std::uint32_t sourceLocation = appendSource(
            result->sourceLocations, bus.source);
        result->buses.push_back({bus.busId, bus.parentId, ordinal, bus.faderDb,
                                 bus.faderAfterSlot, resolved, 0, 0, 0, 0,
                                 sourceLocation, bus.makeUpGainDb,
                                 bus.outputBusVolumeDb});
    }

    for (const auto& [key, send] : sendRows) {
        const std::uint32_t sourceLocation = appendSource(
            result->sourceLocations, send.source);
        result->sends.push_back({send.sourceBusId, send.targetBusId,
                                 send.sendLevelDb, sourceLocation});
    }

    for (auto& bus : result->buses) {
        bus.effectFirst = static_cast<std::uint32_t>(result->effects.size());
        for (const auto& [key, effect] : effectRows) {
            if (effect.busId != bus.busId)
                continue;
            CompiledBusEffectV1 compiled{
                effect.busId, effect.effectId, effect.slot, effect.effectType,
                effect.descriptorVintage, effect.bypass ? 1u : 0u, 0, 0,
                appendSource(result->sourceLocations, effect.source)};
            compiled.parameterFirst = static_cast<std::uint32_t>(
                result->effectParameters.size());
            for (std::uint32_t index = 0; index < effect.parameterCount; ++index) {
                const auto parameter = std::find_if(
                    input.effectParameters.begin(), input.effectParameters.end(),
                    [&](const auto& row) {
                        return row.effectId == effect.effectId
                            && row.index == index;
                    });
                if (parameter != input.effectParameters.end()) {
                    const std::uint32_t sourceLocation = appendSource(
                        result->sourceLocations, parameter->source);
                    result->effectParameters.push_back({parameter->index,
                                                         parameter->value,
                                                         sourceLocation});
                }
            }
            compiled.parameterCount = static_cast<std::uint32_t>(
                result->effectParameters.size()) - compiled.parameterFirst;
            result->effects.push_back(compiled);
        }
        bus.effectCount = static_cast<std::uint32_t>(result->effects.size())
            - bus.effectFirst;
    }

    for (auto& bus : result->buses) {
        bus.downmixFirst = static_cast<std::uint32_t>(result->downmix.size());
        const auto authoring = byId.at(bus.busId);
        const auto explicitRow = downmixRows.find(bus.busId);
        if (explicitRow == downmixRows.end()) {
            const std::uint32_t sourceLocation = appendSource(
                result->sourceLocations, authoring.source);
            result->downmix.push_back({bus.busId,
                                       input.compileTargetProfile->mixLayout,
                                       bus.outputLayout,
                                       static_cast<std::uint32_t>(
                                            result->downmixCoefficients.size()),
                                       64, sourceLocation});
            addDefaultMatrix(result->downmixCoefficients,
                             input.compileTargetProfile->mixLayout,
                             bus.outputLayout);
        } else {
            const auto& row = explicitRow->second;
            if (row.sourceLayout != input.compileTargetProfile->mixLayout
                || row.destinationLayout != bus.outputLayout) {
                outcome.diagnostics.push_back(detailDiagnostic(
                    BusDiagnosticCodeV1::InvalidLayoutDownmix, bus.busId,
                    row.destinationLayout, input.projectGeneration, row.source));
                continue;
            }
            const auto begin = input.downmixCoefficients.begin()
                + row.coefficientFirst;
            const std::uint32_t destinationChannels = channelCount(
                row.destinationLayout);
            const std::uint32_t sourceChannels = channelCount(row.sourceLayout);
            for (std::uint32_t r = 0; r < 8; ++r) {
                for (std::uint32_t c = 0; c < 8; ++c) {
                    const float value = begin[r * 8 + c];
                    if ((r >= destinationChannels || c >= sourceChannels)
                        && value != 0.0f)
                        outcome.diagnostics.push_back(diagnostic(
                            BusDiagnosticCodeV1::InvalidLayoutDownmix,
                            bus.busId, {}, input.projectGeneration, row.source));
                }
            }
            const std::uint32_t first = static_cast<std::uint32_t>(
                result->downmixCoefficients.size());
            const std::uint32_t sourceLocation = appendSource(
                result->sourceLocations, row.source);
            result->downmix.push_back({bus.busId, row.sourceLayout,
                                       row.destinationLayout, first, 64,
                                       sourceLocation});
            result->downmixCoefficients.insert(result->downmixCoefficients.end(),
                                               begin, begin + 64);
        }
        bus.downmixCount = static_cast<std::uint32_t>(result->downmix.size())
            - bus.downmixFirst;
    }

    std::uint64_t canonicalCoefficientCount = 0;
    if (!checkedMultiply(result->downmix.size(), 64, canonicalCoefficientCount)
        || result->downmix.size() > input.capacity.maxDownmixRows
        || canonicalCoefficientCount > input.capacity.maxDownmixCoefficients) {
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::InvalidLayoutDownmix,
            static_cast<std::uint64_t>(result->downmix.size()), 0,
            input.projectGeneration));
    }
    if (!outcome.diagnostics.empty()) {
        std::sort(outcome.diagnostics.begin(), outcome.diagnostics.end(),
                  [](const auto& left, const auto& right) {
                      return std::tie(left.code, left.offendingId,
                                      left.relatedId, left.source.documentPath,
                                      left.source.fieldPath)
                          < std::tie(right.code, right.offendingId,
                                      right.relatedId, right.source.documentPath,
                                      right.source.fieldPath);
                  });
        return outcome;
    }

    std::uint32_t sourceDataBytes = 0;
    std::uint64_t fingerprintSize = 0;
    if (!serializedProofSize(*result, result->checkedByteSize,
                             sourceDataBytes)
        || result->checkedByteSize > input.capacity.maxCompiledBytes
        || !checkedFingerprintStreamSize(*result, false,
                                         fingerprintSize)) {
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::CompiledByteCapacity,
            static_cast<std::uint64_t>(result->checkedByteSize), 0,
            input.projectGeneration));
        return outcome;
    }
    std::vector<std::uint8_t> authoredFingerprintStream;
    if (!fingerprintStream(
            *result, false,
            BusPublicationCapabilityVintageV1::FullAuthoredProperties,
            authoredFingerprintStream)
        || authoredFingerprintStream.size() != fingerprintSize) {
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::CompiledByteCapacity,
            static_cast<std::uint64_t>(fingerprintSize), 0,
            input.projectGeneration));
        return outcome;
    }
    result->authoredBusFingerprint = fnv1a(authoredFingerprintStream);
    result->deterministicBytes = SerializeCompiledBusResultV1(*result);
    if (result->deterministicBytes.size() != result->checkedByteSize) {
        outcome.diagnostics.push_back(numericDiagnostic(
            BusDiagnosticCodeV1::CompiledByteCapacity,
            static_cast<std::uint64_t>(result->checkedByteSize), 0,
            input.projectGeneration));
        return outcome;
    }
    outcome.result = std::move(result);
    return outcome;
}

PublishedBusProjectionV1 BuildPublishedBusProjectionV1(
    const CompiledBusResultV1& result,
    BusPublicationCapabilityVintageV1 capabilityVintage)
{
    PublishedBusProjectionV1 projection;
    projection.capabilityVintage = capabilityVintage;
    projection.projectGeneration = result.projectGeneration;
    projection.identityIndex = result.identityIndex;
    projection.compileTargetProfile = result.compileTargetProfile;
    projection.compileTargetProfileFingerprint = result.compileTargetProfileFingerprint;
    projection.buses = result.buses;
    projection.sends = result.sends;
    if (capabilityVintage == BusPublicationCapabilityVintageV1::
            TopologyAndVolumeNeutralOtherProperties) {
        for (auto& send : projection.sends) send.sendLevelDb = 0.0f;
    }
    if (capabilityVintage == BusPublicationCapabilityVintageV1::FullAuthoredProperties) {
        projection.effects = result.effects;
        projection.effectParameters = result.effectParameters;
        projection.downmix = result.downmix;
        projection.downmixCoefficients = result.downmixCoefficients;
    } else {
        projection.effects.clear();
        projection.effectParameters.clear();
        projection.downmix.clear();
        projection.downmixCoefficients.clear();
        for (auto& bus : projection.buses) {
            if (capabilityVintage != BusPublicationCapabilityVintageV1::
                    TopologyAndVolumeNeutralOtherProperties &&
                capabilityVintage != BusPublicationCapabilityVintageV1::
                    TopologyVolumeAndSendNeutralOtherProperties) {
                bus.faderDb = 0.0f;
                bus.makeUpGainDb = 0.0f;
                bus.outputBusVolumeDb = 0.0f;
            } else {
                bus.faderAfterSlot = 8;
            }
            bus.effectFirst = bus.effectCount = 0;
            bus.downmixFirst = static_cast<std::uint32_t>(projection.downmix.size());
            bus.downmixCount = 1;
            const std::uint32_t destination = projection.compileTargetProfile
                ? projection.compileTargetProfile->mixLayout : bus.outputLayout;
            bus.outputLayout = destination;
            const std::uint32_t first = static_cast<std::uint32_t>(
                projection.downmixCoefficients.size());
            projection.downmix.push_back({
                bus.busId,
                projection.compileTargetProfile
                    ? projection.compileTargetProfile->mixLayout : destination,
                destination, first, 64});
            if (projection.compileTargetProfile)
                addDefaultMatrix(projection.downmixCoefficients,
                                 projection.compileTargetProfile->mixLayout,
                                 destination);
            else
                addDefaultMatrix(projection.downmixCoefficients,
                                 destination, destination);
        }
    }
    CompiledBusV1 outputRoot;
    outputRoot.busId = harkenstudio::kMasterBusObjectId;
    outputRoot.feedsFirstOrdinal = static_cast<std::uint32_t>(projection.buses.size());
    outputRoot.outputLayout = projection.compileTargetProfile
        ? projection.compileTargetProfile->deviceLayout : harkenstudio::kChannelLayoutDefault;
    outputRoot.downmixFirst = static_cast<std::uint32_t>(projection.downmix.size());
    outputRoot.downmixCount = 1;
    projection.downmix.push_back({
        outputRoot.busId,
        projection.compileTargetProfile
            ? projection.compileTargetProfile->mixLayout : outputRoot.outputLayout,
        outputRoot.outputLayout,
        static_cast<std::uint32_t>(projection.downmixCoefficients.size()), 64});
    addDefaultMatrix(projection.downmixCoefficients,
                     projection.compileTargetProfile
                         ? projection.compileTargetProfile->mixLayout : outputRoot.outputLayout,
                     outputRoot.outputLayout);
    projection.buses.push_back(outputRoot);
    std::vector<std::uint8_t> publishedFingerprintStream;
    if (!fingerprintStream(projection, true, capabilityVintage,
                           publishedFingerprintStream))
        return projection;
    projection.fingerprint = fnv1a(publishedFingerprintStream);
    return projection;
}

} // namespace harkenbuscompiler
