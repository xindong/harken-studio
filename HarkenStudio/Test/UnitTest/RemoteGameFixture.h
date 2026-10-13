#pragma once

#include "UnitTest/ObjectIdTestSupport.h"
#include "PackFoundationFixture.h"
#include "harken/Harken.h"
#include "harken/core/MediaContentRevision.h"
#include "harken/core/VariableExecutionFingerprint.h"

#include <bit>
#include <cmath>

namespace harkentest::remote {

inline constexpr VnObjectId kPlayAction = ObjectId(4101);
inline constexpr VnObjectId kSound = ObjectId(3101);
inline constexpr VnObjectId kVariable = ObjectId(7);
inline constexpr VnObjectId kEffect{0x7390327849284a10ull, 0xb12592a104932b6eull};
inline constexpr std::array<std::uint8_t, 16> kProjectGuid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

struct GameFingerprints {
    std::uint64_t sound = 0;
    std::uint64_t action = 0;
    std::uint64_t variable = 0;
};

inline bool WriteGamePacks(const std::filesystem::path& directory, GameFingerprints* fingerprints = nullptr) {
    using namespace harken;
    using namespace packfoundation;
    std::vector<float> samples(48000 * 10);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = 0.002f * std::sin(static_cast<float>(i) * 0.0575958653f);
    }
    const float* planes[] = {samples.data()};
    std::uint64_t revision = 0;
    if (!ComputeMediaPcmContentRevisionV1(48000, 1, samples.size(), planes, 1, &revision)) {
        return false;
    }
    PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind = playableproperties::ContentPropertyKindV1::Sound;
    property.stableId = harkentest::ObjectId(kSound);
    property.contentRevision = revision;
    property.routeStableId = VN_BUS_MASTER;
    property.compatibilityFingerprint = 1;
    property.payload.presentMask = kResolvedSectionGroupPropertyMaskV1;
    property.payload.values[3].bits = 1;
    property.payload.values[6].objectId = VN_BUS_MASTER;
    property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    EffectParamV1 gainParameter{0, std::bit_cast<std::uint32_t>(0.0f)};
    EffectNodeV1 gainEffect{1, VN_EFFECT_GAIN, 1, &gainParameter, kEffect};
    property.payload.presentMask |= UINT64_C(0x8000);
    property.payload.effectNodeCount = 1;
    property.payload.effectNodes = &gainEffect;
    property.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(property);
    ActionStepProgramViewV1 step{};
    step.opcode = ActionOpcodeV1::Play;
    step.target.kind = ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::ObjectId(kSound);
    step.target.expectedProgramSemanticFingerprint = property.programSemanticFingerprint;
    SetAbsoluteTime(&step.delay);
    SetAbsoluteTime(&step.fade);
    SetAbsoluteTime(&step.cooldown);
    ActionProgramViewV1 action{};
    action.actionStableId = harkentest::ObjectId(kPlayAction);
    action.steps = &step;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint = ComputeActionProgramFingerprintV1(action);
    VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(kVariable);
    variable.kind = VariableKindV1::Smooth;
    variable.rangeMin = 0;
    variable.rangeMax = 1;
    if (fingerprints) {
        fingerprints->sound = property.programSemanticFingerprint;
        fingerprints->action = action.programSemanticFingerprint;
        fingerprints->variable = ComputeVariableDefinitionFingerprintV1(variable.variableId,
            static_cast<std::uint32_t>(variable.kind), 1u,
            variable.rangeMin, variable.rangeMax, variable.defaultValue);
    }
    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = ObjectIdFromBytes(kProjectGuid);
    world.identity.capabilityMask = kWorldCapabilityPlayableProperties |
                                    kWorldCapabilityActionDefinitions |
                                    kWorldCapabilityVariableDefinitions;
    world.playableProperties = &property;
    world.playablePropertyCount = 1;
    world.actions = &action;
    world.actionCount = 1;
    world.variables = &variable;
    world.variableCount = 1;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    if (!EncodeCurrentWorldShippingBody(world, body)) {
        return false;
    }
    PackCanonicalPartitionHeader partition;
    partition.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    partition.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    partition.bodyBytes = body.size();
    partition.bodyChecksum = Fnv1a64(body.data(), body.size());
    partition.packStableId = kResidentPackStableId;
    partition.partitionRevision = 1;
    partition.ownedDefinitionCount = 3;
    ApplyCanonicalBusTriple(&partition, DefaultFixtureBusTriple());
    std::vector<std::uint8_t> payload(kCanonicalDefinitionPartitionHeaderV2Bytes + body.size());
    SerializePackCanonicalPartitionHeaderV2(partition, payload.data());
    partition.headerChecksum = Fnv1a64(payload.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(partition, payload.data());
    std::memcpy(payload.data() + kCanonicalDefinitionPartitionHeaderV2Bytes, body.data(),
                body.size());
    ImageBuilder content(kPackKindCanonicalContent);
    content.AddEntry(kPackEntryKindPackRequirementBlock, kResidentPackStableId,
                     RequirementPayload(kResidentPackStableId, 1, 1, 3, body.size()));
    content.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kResidentPackStableId,
                     std::move(payload));
    content.AddMedia(kSound, std::move(samples));
    const auto contentBytes = content.Build();
    std::vector<PackInitCatalogRow> rows(2);
    rows[0].stableId = harkentest::ObjectId(kSound);
    rows[0].identityClass = kInitCatalogIdentityClassPlayable;
    rows[1].stableId = harkentest::ObjectId(kPlayAction);
    rows[1].identityClass = kInitCatalogIdentityClassAction;
    for (auto& row : rows) {
        row.ownerPackStableId = kResidentPackStableId;
        row.expectedContentHash = Fnv1a64(contentBytes.data(), contentBytes.size());
        if (!FillLocator(&row, "Content.pack")) {
            return false;
        }
    }
    PackInitCatalogHeader catalog;
    catalog.schemaVersion = 1;
    catalog.storageModelVersion = 1;
    catalog.projectBuildId = 0x52454d4f5445;
    catalog.platformId = 1;
    catalog.languageId = 1;
    catalog.totalDefinitionCount = 3;
    catalog.catalogEntryCount = 2;
    catalog.startupPackCount = 1;
    catalog.recommendedCatalogCapacity = 2;
    if (!InitCatalogBytes(2, 1, &catalog.catalogBytes)) {
        return false;
    }
    catalog.recommendedCatalogBytes = catalog.catalogBytes;
    std::uint64_t initBodyBytes = 0;
    auto initPartition = PartitionPayload(kInitPackStableId, 1, 0, 0, &initBodyBytes,
        64, ObjectIdFromBytes(kProjectGuid));
    if (initPartition.empty()) {
        return false;
    }
    ImageBuilder init(kPackKindInit);
    PackGameplayCatalogHeader gameplay;
    gameplay.projectGuid = kProjectGuid;
    gameplay.initPackStableId = kInitPackStableId;
    gameplay.initContentRevision = 1;
    PackGameplayCatalogActiveDraft callable;
    callable.actionStableId = harkentest::ObjectId(kPlayAction);
    callable.nameBindingRevision = 1;
    callable.callableFingerprint = 901;
    callable.ownerPackStableId = kResidentPackStableId;
    std::uint8_t closure[16];
    StoreLE64(closure, kResidentPackStableId);
    StoreLE64(closure + 8, 1);
    callable.providerClosureFingerprint = Fnv1a64(closure, sizeof(closure));
    callable.name = "Play Remote Probe";
    std::vector<std::uint8_t> gameplayPayload;
    if (!EncodeGameplayCatalog(gameplay, {callable}, {}, &gameplayPayload)) return false;
    init.AddEntry(kPackEntryKindGameplayCatalog, 0, std::move(gameplayPayload));
    init.AddEntry(kPackEntryKindInitRoutingCatalog, 0,
                  CatalogPayload(catalog, rows, {kResidentPackStableId}));
    init.AddEntry(kPackEntryKindPackRequirementBlock, kInitPackStableId,
                  RequirementPayload(kInitPackStableId, 1, 1, 0, initBodyBytes));
    init.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitPackStableId,
                  std::move(initPartition));
    AppendNeutralMasterSnapshotV1(&init);
    return WriteFile(directory / "Init.pack", init.Build()) &&
           WriteFile(directory / "Content.pack", contentBytes);
}

}
