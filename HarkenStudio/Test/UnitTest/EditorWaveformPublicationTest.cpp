#include <harken/Harken.h>
#include "UnitTest/ObjectIdTestSupport.h"
#include "EditorWaveformPublication.h"
#include "JsonReader.h"
#include "JsonWriter.h"
#include "UnitTest/PackFoundationFixture.h"
#include "harken/core/MediaContentRevision.h"
#include "harken/media/PackLogicImage.h"

#include <gtest/gtest.h>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>

namespace {
using namespace harken;
using namespace harkenstudiobuild::tooling;
using harkenhost::JsonValue;
using harkenhost::JsonWriter;
using harkentest::packfoundation::ImageBuilder;

std::vector<std::uint8_t> Partition(std::uint64_t id, ContainerWorldInstallViewV1 world) {
    world.identity.projectId = {0x1111111111111111ULL, 0x1111111111111111ULL};
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV5(world);
    std::vector<std::uint8_t> body;
    EXPECT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    PackCanonicalPartitionHeader header{};
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = id;
    header.partitionRevision = 1;
    header.ownedDefinitionCount = world.actionCount + world.playablePropertyCount;
    harkentest::packfoundation::ApplyCanonicalBusTriple(
        &header, harkentest::packfoundation::DefaultFixtureBusTriple());
    std::vector<std::uint8_t> result(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeaderV2(header, result.data());
    header.headerChecksum = Fnv1a64(result.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, result.data());
    std::memcpy(result.data() + header.headerBytes, body.data(), body.size());
    return result;
}

void Rehash(std::vector<std::uint8_t>& bytes) {
    PackV2Header header{};
    ASSERT_TRUE(ParsePackV2Header(bytes.data(), &header));
    header.fileBytes = bytes.size();
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        auto* at = bytes.data() + header.directoryOffset + i * kPackV2DirectoryEntryBytes;
        auto entry = ParsePackV2DirectoryEntry(at);
        entry.payloadChecksum = Fnv1a64(bytes.data() + entry.offset, entry.size);
        SerializePackV2DirectoryEntry(entry, at);
    }
    header.directoryChecksum = Fnv1a64(bytes.data() + header.directoryOffset, header.entryCount * kPackV2DirectoryEntryBytes);
    SerializePackV2Header(header, bytes.data());
}

struct Fixture {
    std::string manifest;
    std::vector<EditorMetadataPackBytes> packs;
    std::string output;
    std::string error;
    std::string name = "Play_Golden";
    bool sharedMedia = false;
    PackGameplayCatalogHeader catalog{};
    PackGameplayCatalogActiveDraft row{};
    std::vector<PackGameplayCatalogActiveDraft> additionalRows;

    explicit Fixture(std::vector<std::vector<float>> channels = {{0, 0.25f, -0.5f, 1}},
        std::function<void(PlayablePropertyProgramViewV1&, std::vector<ActionStepProgramViewV1>&)> edit = {},
        bool loopMedia = false, bool omitProperty = false, bool legacyRouting = false) {
        const auto frames = channels[0].size();
        std::vector<const float*> pointers;
        for (const auto& channel : channels) { EXPECT_EQ(channel.size(), frames); pointers.push_back(channel.data()); }
        const std::uint64_t loopStartFrame = loopMedia ? 1 : 0;
        const std::uint64_t loopEndFrame = loopMedia ? frames - 1 : 0;
        std::uint64_t revision = 0;
        EXPECT_TRUE(ComputeMediaPcmContentRevisionV1(
            48000, channels.size(), frames, pointers.data(), 1, &revision,
            loopStartFrame, loopEndFrame));
        PlayablePropertyProgramViewV1 property{};
        property.contentPropertyKind = playableproperties::ContentPropertyKindV1::Sound;
        property.stableId = harkentest::ObjectId(3); property.contentRevision = revision; property.routeStableId = VN_BUS_MASTER; property.compatibilityFingerprint = 3;
        property.payload.presentMask = kResolvedSectionGroupPropertyMaskV1;
        property.payload.values[3].bits = 1;
        property.payload.values[6].objectId = VN_BUS_MASTER;
        property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
        std::vector<ActionStepProgramViewV1> steps(1);
        steps[0].target.stableId = harkentest::ObjectId(3);
        harkentest::packfoundation::SetAbsoluteTime(&steps[0].delay);
        harkentest::packfoundation::SetAbsoluteTime(&steps[0].fade);
        harkentest::packfoundation::SetAbsoluteTime(&steps[0].cooldown);
        if (edit) edit(property, steps);
        property.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(property);
        for (auto& step : steps) step.target.expectedProgramSemanticFingerprint = property.programSemanticFingerprint;
        ActionProgramViewV1 action{};
        action.actionStableId = harkentest::ObjectId(4); action.steps = steps.data(); action.stepCount = steps.size();
        action.expandedStepOccurrenceCount = steps.size(); action.expandedFiringCount = 1;
        for (const auto& step : steps) if (step.delay.numerator != 0) action.directFeatureMask |= 1;
        action.transitiveFeatureMask = action.directFeatureMask;
        action.programSemanticFingerprint = ComputeActionProgramFingerprintV1(action);
        ContainerWorldInstallViewV1 world{};
        world.identity.capabilityMask = kWorldCapabilityActionDefinitions |
            (omitProperty ? 0 : kWorldCapabilityPlayableProperties);
        world.actions = &action; world.actionCount = 1;
        world.playableProperties = omitProperty ? nullptr : &property; world.playablePropertyCount = omitProperty ? 0 : 1;
        auto partition = Partition(22, world);
        ImageBuilder content(kPackKindCanonicalContent);
        content.AddEntry(kPackEntryKindPackRequirementBlock, 22,
            harkentest::packfoundation::RequirementPayload(22, 1, 1, 1 + world.playablePropertyCount, partition.size()));
        content.AddEntry(kPackEntryKindCanonicalDefinitionPartition, 22, partition);
        PackDependencyEdge dependency{};
        dependency.prerequisitePackStableId = 11; dependency.expectedPrerequisiteContentRevision = 1;
        dependency.edgeKind = kPackDependencyEdgeKindInit; dependency.required01 = 1;
        harkentest::packfoundation::ApplyCanonicalBusTriple(
            &dependency, harkentest::packfoundation::DefaultFixtureBusTriple());
        std::vector<std::uint8_t> edge(kPackDependencyEdgeV2Bytes);
        SerializePackDependencyEdgeV2(dependency, edge.data());
        content.AddEntry(kPackEntryKindPackDependencyEdge, 0, edge);
        content.AddEntry(kPackEntryKindMedia, harkentest::ObjectId(3), std::vector<std::uint8_t>(kPackTocEntryBytes));
        auto bytes = content.Build();
        const auto blob = harkentest::packfoundation::AlignUp(bytes.size(), kPackBlobAlign);
        bytes.resize(blob + frames * channels.size() * sizeof(float));
        for (std::size_t c = 0; c < channels.size(); ++c) for (std::size_t f = 0; f < frames; ++f)
            StoreLE32(bytes.data() + blob + (c * frames + f) * 4, std::bit_cast<std::uint32_t>(channels[c][f]));
        PackTocEntry media{};
        media.soundId = harkentest::ObjectId(3); media.sampleRate = 48000; media.channelCount = channels.size(); media.frameCount = frames;
        media.blobOffset = blob; media.blobBytes = frames * channels.size() * 4;
        media.blobChecksum = Fnv1a64(bytes.data() + blob, media.blobBytes);
        media.flags = loopMedia ? kPackFlagLoopable : 0;
        media.loopStartFrame = loopStartFrame;
        media.loopEndFrame = loopEndFrame;
        auto record = ParsePackV2DirectoryEntry(bytes.data() + kPackV2HeaderBytes + 3 * kPackV2DirectoryEntryBytes);
        record.vintage = kPackV2SchemaVintage;
        SerializePackV2DirectoryEntry(record, bytes.data() + kPackV2HeaderBytes + 3 * kPackV2DirectoryEntryBytes);
        SerializePackTocEntry(media, bytes.data() + record.offset);
        Rehash(bytes);

        row.actionStableId = harkentest::ObjectId(4); row.nameBindingRevision = 1;
        row.name = name; row.ownerPackStableId = 22;
        std::vector<std::uint8_t> callable(30 + name.size());
        StoreObjectId(callable.data(), row.actionStableId);
        StoreLE64(callable.data() + 16, 1); StoreLE16(callable.data() + 24, name.size());
        std::memcpy(callable.data() + 26, name.data(), name.size());
        StoreLE32(callable.data() + 26 + name.size(), kInitCatalogIdentityClassAction);
        row.callableFingerprint = Fnv1a64(callable.data(), callable.size());
        std::array<std::uint8_t, 32> providers{};
        StoreLE64(providers.data(), 11); StoreLE64(providers.data() + 8, 1);
        StoreLE64(providers.data() + 16, 22); StoreLE64(providers.data() + 24, 1);
        row.providerClosureFingerprint = Fnv1a64(providers.data(), providers.size());
        catalog.projectGuid.fill(0x11); catalog.initPackStableId = 11; catalog.initContentRevision = 1;
        std::vector<std::uint8_t> payload;
        EXPECT_TRUE(EncodeGameplayCatalog(catalog, {row}, {}, &payload));
        LoadPackGameplayCatalogHeader(payload.data(), &catalog);
        ImageBuilder init(kPackKindInit);
        auto initPartition = Partition(11, {});
        init.AddEntry(kPackEntryKindPackRequirementBlock, 11,
            harkentest::packfoundation::RequirementPayload(11, 1, 1, 0, initPartition.size()));
        init.AddEntry(kPackEntryKindCanonicalDefinitionPartition, 11, initPartition);
        harkentest::packfoundation::AppendNeutralMasterSnapshotV1(&init);
        init.AddEntry(kPackEntryKindGameplayCatalog, 0, payload);
        PackInitCatalogHeader routingHeader{};
        routingHeader.schemaVersion = 1; routingHeader.storageModelVersion = 1;
        routingHeader.projectBuildId = 7; routingHeader.platformId = 1;
        routingHeader.totalDefinitionCount = 2; routingHeader.catalogEntryCount = 2;
        routingHeader.recommendedCatalogCapacity = 2;
        EXPECT_TRUE(InitCatalogBytes(2, 0, &routingHeader.catalogBytes));
        routingHeader.recommendedCatalogBytes = routingHeader.catalogBytes;
        std::vector<std::uint8_t> routing(routingHeader.catalogBytes);
        for (std::uint32_t index = 0; index < 2; ++index) {
            PackInitCatalogRow route{};
            route.stableId = harkentest::ObjectId(index == 0 ? 3 : 4);
            route.identityClass = index == 0 ? kInitCatalogIdentityClassPlayable : kInitCatalogIdentityClassAction;
            route.ownerPackStableId = 22;
            route.expectedContentHash = Fnv1a64(bytes.data(), bytes.size());
            constexpr char locator[] = "action_4.pack";
            route.locatorByteCount = sizeof(locator);
            std::memcpy(route.relativeLocator.data(), locator, sizeof(locator));
            SerializePackInitCatalogRow(route, routing.data() + kInitCatalogHeaderBytes + index * kInitCatalogRowBytes);
        }
        routingHeader.checksum = Fnv1a64(routing.data() + kInitCatalogHeaderBytes,
            routing.size() - kInitCatalogHeaderBytes);
        SerializePackInitCatalogHeader(routingHeader, routing.data());
        if (legacyRouting) init.AddEntry(kPackEntryKindInitRoutingCatalog, 0, routing);
        packs = {{11, init.Build()}, {22, std::move(bytes)}};
        CheckCanonical();
        RefreshManifest();
    }

    void RefreshManifest() {
        JsonWriter json;
        const auto text = [&](const char* key, const std::string& value) { json.Key(key); json.ValueString(value); };
        const auto id = [&](const char* key, std::uint64_t value) { text(key, JsonWriter::DecimalU64(value)); };
        const auto number = [&](const char* key, std::uint64_t value) { json.Key(key); json.ValueU64(value); };
        json.BeginObject();
        number("schema_version", 3); text("platformId", "33333333-3333-3333-3333-333333333333");
        text("platformName", "Windows"); text("basePlatform", "Windows"); text("language", ""); id("initPackStableId", 11);
        json.Key("packs"); json.BeginArray();
        for (const auto& pack : packs) {
            json.BeginObject();
            text("name", pack.packStableId == 11 ? "Init" : "Play_Golden");
            text("path", pack.packStableId == 11 ? "Init.pack" : pack.packStableId == 22 ? "action_4.pack" : "shared_33.pack");
            text("platformId", "33333333-3333-3333-3333-333333333333");
            id("packStableId", pack.packStableId); id("contentHash", Fnv1a64(pack.bytes.data(), pack.bytes.size()));
            number("packKind", pack.packStableId == 11 ? kPackKindInit : kPackKindCanonicalContent);
            number("contentRevision", 1); text("language", ""); id("variantKey", 0);
            json.Key("dependencies"); json.BeginArray(); if (pack.packStableId != 11) json.ValueString("11");
            if (sharedMedia && pack.packStableId == 22) json.ValueString("33"); json.EndArray();
            json.EndObject();
        }
        json.EndArray();
        json.Key("gameplayCatalog"); json.BeginObject();
        number("schema", 2); text("projectId", "11111111-1111-1111-1111-111111111111");
        id("initPackStableId", 11); number("initContentRevision", 1); id("digest", catalog.digest);
        json.Key("actions"); json.BeginArray(); json.BeginObject();
        text("name", name); text("actionStableId", harken::ObjectIdString(harkentest::ObjectId(4)));
        id("nameBindingRevision", 1); id("callableFingerprint", row.callableFingerprint);
        id("ownerPackStableId", 22); id("providerClosureFingerprint", row.providerClosureFingerprint);
        json.EndObject();
        for (const auto& extra : additionalRows) {
            json.BeginObject();
            text("name", std::string(extra.name)); text("actionStableId", ObjectIdString(extra.actionStableId));
            id("nameBindingRevision", extra.nameBindingRevision); id("callableFingerprint", extra.callableFingerprint);
            id("ownerPackStableId", extra.ownerPackStableId); id("providerClosureFingerprint", extra.providerClosureFingerprint);
            json.EndObject();
        }
        json.EndArray(); json.Key("retired"); json.BeginArray(); json.EndArray();
        json.EndObject(); json.Key("groups"); json.BeginArray(); json.EndArray(); json.EndObject(); manifest = json.Finish();
    }

    void CheckCanonical() {
        for (const auto& pack : packs) {
            PackCanonicalImage image;
            EXPECT_EQ(ParsePackCanonicalImage(pack.bytes.data(), pack.bytes.size(), &image), VN_OK)
                << "Fixture Pack " << pack.packStableId;
            if (image.hasPartition) {
                CurrentWorldBodyImage world;
                EXPECT_TRUE(world.Decode(image.partitionBody.data(), image.partitionBody.size(),
                    image.partitionHeader.bodyVintage, CurrentWorldDecodeMode::StrictArtifact))
                    << "Fixture world " << pack.packStableId;
            }
        }
    }
    bool Build() { return BuildEditorWaveformPublication(manifest, packs, &output, &error); }
    JsonValue Read() {
        JsonValue json;
        EXPECT_TRUE(harkenhost::ParseJson(output, &json, &error, true)) << error;
        return json;
    }
};

struct RandomFixture : Fixture {
    std::vector<std::string> actionNames;
    // Hazard: 1 container pitch, 2 occurrence loop, 3 ancestor pitch, 4 unsupported mode, 5 missing media.
    explicit RandomFixture(bool shared = false, unsigned count = 3, unsigned hazard = 0, bool budgetRollback = false, bool decodedBudget = false) {
        sharedMedia = shared;
        std::vector<std::vector<float>> pcm{{0.5f, -0.5f, 1, -1}, {0.25f, 0.25f, -0.25f, -0.25f, 0, 0, 0.75f, -0.75f}};
        // Leave room for one source: the next two-source Action must roll back atomically.
        constexpr unsigned budgetPrefixSources = kEditorWaveformMaxBytes / (1024 + 8) - 1;
        constexpr unsigned budgetPrefixContainers = (budgetPrefixSources + 63) / 64;
        const unsigned soundCount = decodedBudget ? budgetPrefixSources + 3 : budgetRollback ? 13 : 2;
        const unsigned containerCount = decodedBudget ? budgetPrefixContainers + 1 : 1;
        const unsigned occurrenceCount = decodedBudget ? budgetPrefixSources + 2 : count;
        const unsigned channels = budgetRollback ? VN_MAX_CHANNELS : 1;
        if (budgetRollback) pcm.assign(soundCount, std::vector<float>(2048, 0.25f));
        if (decodedBudget) pcm.assign(soundCount, std::vector<float>(1, 0.0f));
        std::vector<PlayablePropertyProgramViewV1> properties(soundCount + containerCount);
        StructuralAncestorRefV1 ancestor{};
        for (unsigned i = 0; i < soundCount + containerCount; ++i) {
            auto& property = properties[i];
            property.contentPropertyKind = i >= soundCount ? playableproperties::ContentPropertyKindV1::Container : playableproperties::ContentPropertyKindV1::Sound;
            property.stableId = harkentest::ObjectId(decodedBudget ? 100000 + i : i == soundCount ? 5 : i == 0 ? 3 : 6 + i);
            property.authoredStableOrder = i; property.contentRevision = 1;
            property.routeStableId = VN_BUS_MASTER; property.compatibilityFingerprint = i + 1;
            property.payload.presentMask = kResolvedSectionGroupPropertyMaskV1;
            property.payload.values[3].bits = 1; property.payload.values[6].objectId = VN_BUS_MASTER;
            property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
            if (i < soundCount) {
                std::vector<const float*> channel(channels, pcm[i].data());
                EXPECT_TRUE(ComputeMediaPcmContentRevisionV1(i == 0 ? 48000 : 24000, channels, pcm[i].size(), channel.data(), 1, &property.contentRevision));
            }
            if (i == soundCount && hazard == 1) {
                property.payload.presentMask |= 2; property.payload.values[1].bits = std::bit_cast<std::uint32_t>(12.0f);
            }
            if (i == soundCount && hazard == 3) {
                ancestor.stableId = harkentest::ObjectId(99);
                ancestor.payload.presentMask = 2;
                ancestor.payload.values[1].bits = std::bit_cast<std::uint32_t>(12.0f);
                ancestor.payload.values[3].bits = 1;
                ancestor.payload.values[11].bits = ancestor.payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
                ancestor.programSemanticFingerprint = 42;
                property.structuralAncestorCount = 1; property.structuralAncestors = &ancestor;
            }
            property.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(property);
        }
        std::vector<ContainerChildProgramViewV2> children(occurrenceCount);
        std::vector<PlayableOccurrencePropertyViewV1> occurrences(occurrenceCount);
        for (unsigned i = 0; i < occurrenceCount; ++i) {
            auto& child = children[i]; const auto& property = properties[decodedBudget ? i : i % soundCount];
            const unsigned parent = decodedBudget ? (i < budgetPrefixSources ? i / 64 : budgetPrefixContainers) : 0;
            const unsigned ordinal = decodedBudget ? (i < budgetPrefixSources ? i % 64 : i - budgetPrefixSources) : i;
            child.stableChildOrdinal = ordinal + 1; child.authoredStableOrder = ordinal; child.frequencyWeight = 1;
            if (hazard == 4) child.frequencyWeight = 0;
            child.playable = {PlayableKindV1::Sound, 0, property.stableId, property.programSemanticFingerprint,
                property.contentRevision, property.routeStableId, property.compatibilityFingerprint};
            auto& occurrence = occurrences[i];
            occurrence.parentKind = playableproperties::ContentPropertyKindV1::Container;
            occurrence.parentStableId = properties[soundCount + parent].stableId;
            occurrence.parentProgramSemanticFingerprint = properties[soundCount + parent].programSemanticFingerprint;
            occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal; occurrence.childPlayable = child.playable;
            occurrence.payload.values[3].bits = 1; occurrence.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
            occurrence.payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
            if (hazard == 2 && i == 1) { occurrence.payload.presentMask |= 8; occurrence.payload.values[3].bits = 2; }
            occurrence.occurrenceSemanticFingerprint = ComputePlayableOccurrenceFingerprintV1(occurrence);
        }
        std::vector<ContainerProgramViewV3> containers(containerCount);
        for (unsigned i = 0; i < containerCount; ++i) {
            auto& entry = containers[i];
            entry.authoredStableOrder = i;
            entry.containerStableId = properties[soundCount + i].stableId;
            entry.programSemanticFingerprint = properties[soundCount + i].programSemanticFingerprint;
            entry.mode = hazard == 4 ? ContainerModeV1::Sequence : ContainerModeV1::Random;
            const unsigned offset = decodedBudget ? std::min(i * 64, budgetPrefixSources) : 0;
            entry.children = children.data() + offset;
            entry.childCount = decodedBudget ? (i < budgetPrefixContainers ? std::min(64u, budgetPrefixSources - offset) : 2) : count;
        }
        const auto& container = containers[0];
        ActionStepProgramViewV1 step{};
        step.target.kind = ActionTargetKindV1::Container; step.target.stableId = container.containerStableId;
        step.target.expectedProgramSemanticFingerprint = container.programSemanticFingerprint;
        harkentest::packfoundation::SetAbsoluteTime(&step.delay);
        harkentest::packfoundation::SetAbsoluteTime(&step.fade);
        harkentest::packfoundation::SetAbsoluteTime(&step.cooldown);
        ActionProgramViewV1 action{}; action.actionStableId = row.actionStableId;
        action.steps = &step; action.stepCount = 1; action.expandedStepOccurrenceCount = action.expandedFiringCount = 1;
        action.programSemanticFingerprint = ComputeActionProgramFingerprintV1(action);
        std::vector<ActionProgramViewV1> actions(decodedBudget ? containerCount + 2 : 2, action);
        std::vector<ActionStepProgramViewV1> manySteps(actions.size(), step);
        ActionStepProgramViewV1 directStep = step;
        if (budgetRollback) {
            auto extra = row;
            extra.actionStableId = harkentest::ObjectId(40); extra.name = "Play_AfterBudget";
            std::vector<std::uint8_t> callable(30 + extra.name.size());
            StoreObjectId(callable.data(), extra.actionStableId); StoreLE64(callable.data() + 16, 1);
            StoreLE16(callable.data() + 24, extra.name.size());
            std::memcpy(callable.data() + 26, extra.name.data(), extra.name.size());
            StoreLE32(callable.data() + 26 + extra.name.size(), kInitCatalogIdentityClassAction);
            extra.callableFingerprint = Fnv1a64(callable.data(), callable.size());
            additionalRows.push_back(extra);
            directStep.target.kind = ActionTargetKindV1::Sound;
            directStep.target.stableId = properties[0].stableId;
            directStep.target.expectedProgramSemanticFingerprint = properties[0].programSemanticFingerprint;
            actions[1].actionStableId = extra.actionStableId; actions[1].steps = &directStep;
            actions[1].programSemanticFingerprint = ComputeActionProgramFingerprintV1(actions[1]);
        }
        if (decodedBudget) {
            actionNames.resize(actions.size());
            for (unsigned i = 0; i < actions.size(); ++i) {
                auto& target = manySteps[i].target;
                if (i < containerCount) {
                    target.stableId = containers[i].containerStableId;
                    target.expectedProgramSemanticFingerprint = containers[i].programSemanticFingerprint;
                } else {
                    const auto& property = properties[i == containerCount ? soundCount - 1 : 0];
                    target.kind = ActionTargetKindV1::Sound;
                    target.stableId = property.stableId;
                    target.expectedProgramSemanticFingerprint = property.programSemanticFingerprint;
                }
                actions[i].steps = &manySteps[i];
                if (i != 0) {
                    auto extra = row;
                    extra.actionStableId = harkentest::ObjectId(1000 + i);
                    actionNames[i] = "Play_Budget_" + std::to_string(i);
                    extra.name = actionNames[i];
                    std::vector<std::uint8_t> callable(30 + extra.name.size());
                    StoreObjectId(callable.data(), extra.actionStableId); StoreLE64(callable.data() + 16, 1);
                    StoreLE16(callable.data() + 24, extra.name.size());
                    std::memcpy(callable.data() + 26, extra.name.data(), extra.name.size());
                    StoreLE32(callable.data() + 26 + extra.name.size(), kInitCatalogIdentityClassAction);
                    extra.callableFingerprint = Fnv1a64(callable.data(), callable.size());
                    additionalRows.push_back(extra);
                    actions[i].actionStableId = extra.actionStableId;
                }
                actions[i].programSemanticFingerprint = ComputeActionProgramFingerprintV1(actions[i]);
            }
        }
        ContainerWorldInstallViewV1 world{};
        world.identity.capabilityMask = kWorldCapabilityActionDefinitions | kWorldCapabilityPlayableProperties |
            (hazard == 4 ? kWorldCapabilitySequence : kWorldCapabilityRandomDraw);
        world.playableProperties = shared ? &properties[soundCount] : properties.data(); world.playablePropertyCount = shared ? 1 : properties.size();
        world.playableOccurrences = occurrences.data(); world.playableOccurrenceCount = occurrenceCount;
        world.containers = containers.data(); world.containerCount = containers.size(); world.actions = actions.data(); world.actionCount = decodedBudget ? actions.size() : budgetRollback ? 2 : 1;
        world.capacity.maxContainerPrograms = std::max(world.capacity.maxContainerPrograms, world.containerCount);
        world.capacity.maxPlayablePropertyPrograms = std::max(world.capacity.maxPlayablePropertyPrograms, world.playablePropertyCount);
        world.capacity.maxPlayableOccurrencePropertyRows = std::max(world.capacity.maxPlayableOccurrencePropertyRows, world.playableOccurrenceCount);
        world.capacity.maxChildrenPerContainer = std::max(64u, count);
        world.capacity.maxActionDefinitions = world.actionCount; world.capacity.maxActionStepsPerDefinition = 1;
        world.capacity.maxActionStepsTotal = world.actionCount; world.capacity.maxActionExpandedWork = 1;
        const auto build = [&](std::uint64_t id, ContainerWorldInstallViewV1 body, bool includeMedia, bool dependency) {
            auto partition = Partition(id, body);
            ImageBuilder content(kPackKindCanonicalContent);
            content.AddEntry(kPackEntryKindPackRequirementBlock, id, harkentest::packfoundation::RequirementPayload(
                id, 1, 1, body.actionCount + body.playablePropertyCount, partition.size()));
            content.AddEntry(kPackEntryKindCanonicalDefinitionPartition, id, partition);
            auto edges = harkentest::packfoundation::DependencyPayload(11, 1, harkentest::packfoundation::DefaultFixtureBusTriple());
            content.AddEntry(kPackEntryKindPackDependencyEdge, 0, edges);
            if (dependency) {
                PackDependencyEdge edge{}; edge.prerequisitePackStableId = 33; edge.expectedPrerequisiteContentRevision = 1;
                edge.edgeKind = kPackDependencyEdgeKindShared; edge.required01 = 1;
                std::vector<std::uint8_t> sharedEdge(kPackDependencyEdgeV2Bytes);
                SerializePackDependencyEdgeV2(edge, sharedEdge.data());
                auto index = content.AddEntry(kPackEntryKindPackDependencyEdge, 0, sharedEdge);
                content.SetRelationOrdinal(index, 1);
            }
            if (includeMedia) for (unsigned i = 0; i < (hazard == 5 ? 1u : soundCount); ++i)
                content.AddEntry(kPackEntryKindMedia, properties[i].stableId, std::vector<std::uint8_t>(kPackTocEntryBytes));
            auto bytes = content.Build();
            if (includeMedia) for (unsigned i = 0; i < (hazard == 5 ? 1u : soundCount); ++i) {
                const auto offset = harkentest::packfoundation::AlignUp(bytes.size(), kPackBlobAlign);
                bytes.resize(offset + pcm[i].size() * channels * 4);
                for (unsigned c = 0; c < channels; ++c) for (unsigned f = 0; f < pcm[i].size(); ++f) StoreLE32(bytes.data() + offset + (c * pcm[i].size() + f) * 4, std::bit_cast<std::uint32_t>(pcm[i][f]));
                PackTocEntry media{}; media.soundId = properties[i].stableId;
                media.frameCount = pcm[i].size(); media.sampleRate = i == 0 ? 48000 : 24000; media.channelCount = channels;
                media.blobOffset = offset; media.blobBytes = pcm[i].size() * channels * 4;
                media.blobChecksum = Fnv1a64(bytes.data() + offset, media.blobBytes);
                auto* directory = bytes.data() + kPackV2HeaderBytes + (3 + (dependency ? 1 : 0) + i) * kPackV2DirectoryEntryBytes;
                auto entry = ParsePackV2DirectoryEntry(directory); entry.vintage = kPackV2SchemaVintage;
                SerializePackV2DirectoryEntry(entry, directory); SerializePackTocEntry(media, bytes.data() + entry.offset);
            }
            Rehash(bytes); return bytes;
        };
        packs[1] = {22, build(22, world, !shared, shared)};
        if (shared) {
            ContainerWorldInstallViewV1 soundWorld{};
            soundWorld.identity.capabilityMask = kWorldCapabilityPlayableProperties;
            soundWorld.playableProperties = properties.data(); soundWorld.playablePropertyCount = soundCount;
            packs.push_back({33, build(33, soundWorld, true, false)});
            std::array<std::uint8_t, 48> providers{};
            StoreLE64(providers.data(), 11); StoreLE64(providers.data() + 8, 1);
            StoreLE64(providers.data() + 16, 22); StoreLE64(providers.data() + 24, 1);
            StoreLE64(providers.data() + 32, 33);
            row.providerClosureFingerprint = Fnv1a64(providers.data(), providers.size());
            std::vector<std::uint8_t> payload;
            EXPECT_TRUE(EncodeGameplayCatalog(catalog, {row}, {}, &payload));
            LoadPackGameplayCatalogHeader(payload.data(), &catalog);
            ImageBuilder init(kPackKindInit); auto part = Partition(11, {});
            init.AddEntry(kPackEntryKindPackRequirementBlock, 11, harkentest::packfoundation::RequirementPayload(11, 1, 1, 0, part.size()));
            init.AddEntry(kPackEntryKindCanonicalDefinitionPartition, 11, part);
            harkentest::packfoundation::AppendNeutralMasterSnapshotV1(&init);
            init.AddEntry(kPackEntryKindGameplayCatalog, 0, payload);
            packs[0] = {11, init.Build()};
        }
        if (budgetRollback || decodedBudget) {
            std::vector<std::uint8_t> payload;
            std::vector<PackGameplayCatalogActiveDraft> allRows{row};
            allRows.insert(allRows.end(), additionalRows.begin(), additionalRows.end());
            EXPECT_TRUE(EncodeGameplayCatalog(catalog, allRows, {}, &payload));
            LoadPackGameplayCatalogHeader(payload.data(), &catalog);
            ImageBuilder init(kPackKindInit); auto part = Partition(11, {});
            init.AddEntry(kPackEntryKindPackRequirementBlock, 11, harkentest::packfoundation::RequirementPayload(11, 1, 1, 0, part.size()));
            init.AddEntry(kPackEntryKindCanonicalDefinitionPartition, 11, part);
            harkentest::packfoundation::AppendNeutralMasterSnapshotV1(&init);
            init.AddEntry(kPackEntryKindGameplayCatalog, 0, payload);
            packs[0] = {11, init.Build()};
        }
        CheckCanonical(); RefreshManifest();
    }
};

TEST(EditorWaveformPublication, RandomIncludesEveryOccurrenceAndDeduplicatesSharedMedia) {
    for (bool shared : {false, true}) {
        SCOPED_TRACE(shared);
        RandomFixture fixture(shared);
        ASSERT_TRUE(fixture.Build()) << fixture.error;
        auto json = fixture.Read();
        const auto& row = json.Find("actions")->items[0];
        ASSERT_EQ(row.Find("kind")->text, "randomCandidates");
        ASSERT_EQ(row.Find("candidates")->items.size(), 3u);
        ASSERT_EQ(json.Find("sources")->items.size(), 2u);
        EXPECT_EQ(row.Find("candidates")->items[0].Find("sourceKey")->text, row.Find("candidates")->items[2].Find("sourceKey")->text);
        EXPECT_NE(row.Find("candidates")->items[0].Find("occurrence")->text, row.Find("candidates")->items[2].Find("occurrence")->text);
        EXPECT_EQ(row.Find("duration")->Find("kind")->text, "bounded");
        for (const auto& source : json.Find("sources")->items)
            EXPECT_EQ(source.Find("ownerPackStableId")->text, shared ? "33" : "22");
        const auto first = fixture.output;
        ASSERT_TRUE(fixture.Build()) << fixture.error; EXPECT_EQ(fixture.output, first);
        if (shared) {
            const auto golden = std::filesystem::path(HARKEN_EDITOR_METADATA_GOLDEN_DIR) / "random";
            for (const auto& [name, expected] : std::vector<std::pair<std::string, std::string>>{
                {"HarkenPackManifest.json", fixture.manifest}, {"HarkenEditorWaveforms.json", fixture.output}}) {
                std::ifstream file(golden / name, std::ios::binary);
                const std::string actual{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
                EXPECT_EQ(actual, expected) << name;
            }
            const auto root = std::filesystem::path(HARKEN_TEMP_DIR) / "editor-waveform-v2-random";
            std::filesystem::create_directories(root);
            std::ofstream(root / "HarkenPackManifest.json", std::ios::binary) << fixture.manifest;
            std::ofstream(root / "HarkenEditorWaveforms.json", std::ios::binary) << fixture.output;
            for (const auto& pack : fixture.packs) {
                const auto path = root / (pack.packStableId == 11 ? "Init.pack" : pack.packStableId == 22 ? "action_4.pack" : "shared_33.pack");
                std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(pack.bytes.data()), pack.bytes.size());
            }
        }
    }
}

TEST(EditorWaveformPublication, RandomDurationChecksContainerOccurrenceAndAncestor) {
    for (unsigned hazard : {1, 2, 3}) {
        SCOPED_TRACE(hazard);
        RandomFixture fixture(true, 3, hazard);
        ASSERT_TRUE(fixture.Build()) << fixture.error;
        auto json = fixture.Read(); const auto& row = json.Find("actions")->items[0];
        EXPECT_EQ(row.Find("kind")->text, "randomCandidates");
        EXPECT_EQ(row.Find("duration")->Find("kind")->text, "unknown");
        EXPECT_EQ(row.Find("candidates")->items.size(), 3u);
    }
}

TEST(EditorWaveformPublication, RandomUnsupportedOrOversizedActionDoesNotPublishPartialSources) {
    for (unsigned hazard : {0, 4, 5}) {
        SCOPED_TRACE(hazard);
        RandomFixture fixture(true, hazard == 0 ? 257 : 3, hazard);
        ASSERT_TRUE(fixture.Build()) << fixture.error;
        auto json = fixture.Read(); const auto& row = json.Find("actions")->items[0];
        EXPECT_EQ(row.Find("kind")->text, "unavailable");
        EXPECT_TRUE(row.Find("candidates")->items.empty());
        EXPECT_TRUE(json.Find("sources")->items.empty());
    }
}

TEST(EditorWaveformPublication, FailedActionReleasesPendingSourceBudgetForLaterAction) {
    RandomFixture fixture(false, 13, 0, true);
    ASSERT_TRUE(fixture.Build()) << fixture.error;
    const auto json = fixture.Read();
    const auto& actions = json.Find("actions")->items;
    ASSERT_EQ(actions.size(), 2u);
    EXPECT_EQ(actions[0].Find("kind")->text, "unavailable");
    EXPECT_EQ(actions[0].Find("sourceReason")->text, "metadata-budget");
    EXPECT_TRUE(actions[0].Find("candidates")->items.empty());
    EXPECT_EQ(actions[1].Find("kind")->text, "directSound");
    EXPECT_EQ(actions[1].Find("candidates")->items.size(), 1u);
    EXPECT_EQ(json.Find("sources")->items.size(), 1u);
    EXPECT_LT(fixture.output.size(), kEditorWaveformMaxBytes);
}

TEST(EditorWaveformPublication, DecodedSourceBudgetIsAtomicAcrossActionsAndSharedWithReader) {
    RandomFixture fixture(false, 64, 0, false, true);
    ASSERT_TRUE(fixture.Build()) << fixture.error;
    const auto json = fixture.Read();
    const auto& actions = json.Find("actions")->items;
    ASSERT_EQ(actions.size(), 257u);
    EXPECT_EQ(actions[254].Find("kind")->text, "unavailable");
    if (const auto* reason = actions[254].Find("sourceReason")) EXPECT_EQ(reason->text, "metadata-budget");
    else ADD_FAILURE() << "Missing metadata-budget diagnosis";
    EXPECT_TRUE(actions[254].Find("candidates")->items.empty());
    EXPECT_EQ(actions[255].Find("kind")->text, "directSound");
    EXPECT_EQ(actions[256].Find("kind")->text, "directSound");
    EXPECT_EQ(json.Find("sources")->items.size(), 16256u);
    std::size_t decodedBytes = 0;
    for (const auto& source : json.Find("sources")->items)
        decodedBytes += std::stoull(source.Find("channelCount")->text) *
            std::stoull(source.Find("bucketCount")->text) * 8 + 1024;
    EXPECT_LE(decodedBytes, kEditorWaveformMaxBytes);
    EXPECT_LT(fixture.output.size(), kEditorWaveformMaxBytes);
    const auto root = std::filesystem::path(HARKEN_TEMP_DIR) / "editor-waveform-v2-budget";
    std::filesystem::create_directories(root);
    std::ofstream(root / "HarkenPackManifest.json", std::ios::binary) << fixture.manifest;
    std::ofstream(root / "HarkenEditorWaveforms.json", std::ios::binary) << fixture.output;
}

TEST(EditorWaveformPublication, DirectSourceAndDurationAreExactAndDeterministic) {
    Fixture fixture;
    ASSERT_TRUE(fixture.Build()) << fixture.error;
    const auto first = fixture.output;
    auto json = fixture.Read();
    const auto& row = json.Find("actions")->items[0];
    EXPECT_EQ(row.Find("duration")->Find("kind")->text, "exact");
    const auto& source = json.Find("sources")->items[0];
    EXPECT_EQ(source.Find("frameCount")->text, "4");
    EXPECT_EQ(source.Find("sampleRate")->text, "48000");
    const auto& values = source.Find("channels")->items[0].Find("min")->items;
    EXPECT_EQ(values[2].text, "-0.5");
    ASSERT_TRUE(fixture.Build()) << fixture.error;
    EXPECT_EQ(fixture.output, first);
}

TEST(EditorWaveformPublication, ChannelsDoNotCancelOrDropRightOnlySignal) {
    for (auto channels : {std::vector<std::vector<float>>{{0, 0}, {1, -1}}, {{1, -1}, {-1, 1}}}) {
        Fixture fixture(channels);
        ASSERT_TRUE(fixture.Build()) << fixture.error;
        auto json = fixture.Read();
        const auto& source = json.Find("sources")->items[0];
        EXPECT_EQ(source.Find("channelCount")->text, "2");
        const auto& rows = source.Find("channels")->items;
        EXPECT_EQ(rows[1].Find("min")->items[0].text, channels[1][0] == 1 ? "1" : "-1");
        EXPECT_EQ(rows[1].Find("max")->items[1].text, channels[1][1] == 1 ? "1" : "-1");
    }
}

TEST(EditorWaveformPublication, NonuniformBucketsIncludeTheLastFrame) {
    std::vector<float> samples(4097, 0); samples.back() = 1;
    Fixture fixture({samples});
    ASSERT_TRUE(fixture.Build()) << fixture.error;
    auto json = fixture.Read();
    const auto& channel = json.Find("sources")->items[0].Find("channels")->items[0];
    EXPECT_EQ(channel.Find("max")->items.size(), 2048);
    EXPECT_EQ(channel.Find("max")->items.back().text, "1");
}

TEST(EditorWaveformPublication, TimingPitchLoopAndSendsPreserveSourceButHideActionDuration) {
    for (int mode = 0; mode < 5; ++mode) {
        SendV1 send{}; send.targetBusId = harkentest::ObjectId(2);
        Fixture fixture({{0, 1, 0, -1}}, [&](auto& property, auto& steps) {
            if (mode == 0) steps[0].delay.numerator = 1000;
            if (mode == 1) { property.payload.presentMask |= 2; property.payload.values[1].bits = std::bit_cast<std::uint32_t>(12.0f); }
            if (mode == 2) { property.payload.presentMask |= 8; property.payload.values[3].bits = 2; }
            if (mode == 3) { property.payload.presentMask |= 128; property.payload.sendCount = 1; property.payload.sends = &send; }
        }, mode == 4);
        ASSERT_TRUE(fixture.Build()) << fixture.error << " mode=" << mode;
        auto json = fixture.Read();
        const auto& row = json.Find("actions")->items[0];
        EXPECT_EQ(row.Find("kind")->text, "directSound");
        EXPECT_EQ(row.Find("duration")->Find("kind")->text, "unknown");
        EXPECT_NE(row.Find("duration")->Find("reason"), nullptr);
    }
}

TEST(EditorWaveformPublication, CompositeAndUnresolvedSourcesAreExplicitlyUnavailable) {
    for (bool crossOwner : {false, true}) {
        Fixture fixture({{0, 1}}, [crossOwner](auto&, auto& steps) {
            if (!crossOwner) { auto second = steps[0]; second.authoredEventIndex = 1; steps.push_back(second); }
        }, false, crossOwner);
        ASSERT_TRUE(fixture.Build()) << fixture.error;
        auto json = fixture.Read();
        const auto& row = json.Find("actions")->items[0];
        EXPECT_EQ(row.Find("kind")->text, "unavailable");
        EXPECT_EQ(row.Find("sourceReason")->text, crossOwner ? "unresolved-source" : "composite-or-non-sound-action");
    }
}

TEST(EditorWaveformPublication, MixedCorruptAndIncompleteInputsCannotPublish) {
    Fixture fixture;
    const auto original = fixture.packs;
    fixture.packs.pop_back(); EXPECT_FALSE(fixture.Build());
    fixture.packs = original; fixture.packs.push_back(original[1]); EXPECT_FALSE(fixture.Build());
    fixture.packs = original; fixture.packs[1].bytes.back() ^= 1; EXPECT_FALSE(fixture.Build());
    fixture.packs = original; fixture.name = "Same_Id_Different_Name"; fixture.RefreshManifest(); EXPECT_FALSE(fixture.Build());
    EXPECT_TRUE(fixture.output.empty());
}

TEST(EditorWaveformPublication, NonfinitePcmWithValidContainerChecksumsIsRejected) {
    Fixture fixture;
    auto& bytes = fixture.packs[1].bytes;
    auto entry = ParsePackV2DirectoryEntry(bytes.data() + kPackV2HeaderBytes + 3 * kPackV2DirectoryEntryBytes);
    auto media = ParsePackTocEntry(bytes.data() + entry.offset);
    StoreLE32(bytes.data() + media.blobOffset, 0x7fc00000U);
    media.blobChecksum = Fnv1a64(bytes.data() + media.blobOffset, media.blobBytes);
    SerializePackTocEntry(media, bytes.data() + entry.offset);
    Rehash(bytes); fixture.RefreshManifest();
    EXPECT_FALSE(fixture.Build());
    EXPECT_NE(fixture.error.find("finite final PCM"), std::string::npos);
}

TEST(EditorWaveformPublication, LegacyRoutingCatalogIsNotACanonicalPublication) {
    Fixture fixture;
    ASSERT_TRUE(fixture.Build()) << fixture.error;
    Fixture legacy({{0, 0.25f, -0.5f, 1}}, {}, false, false, true);
    EXPECT_FALSE(legacy.Build());
    EXPECT_TRUE(legacy.output.empty());
    EXPECT_NE(legacy.error.find("Legacy Init routing"), std::string::npos);
}

TEST(EditorWaveformPublication, InitWithoutKind24SnapshotCannotPublish) {
    Fixture fixture;
    ASSERT_EQ(2u, fixture.packs.size());
    ImageBuilder init(kPackKindInit);
    auto initPartition = Partition(11, {});
    init.AddEntry(kPackEntryKindPackRequirementBlock, 11,
        harkentest::packfoundation::RequirementPayload(11, 1, 1, 0, initPartition.size()));
    init.AddEntry(kPackEntryKindCanonicalDefinitionPartition, 11, initPartition);
    std::vector<std::uint8_t> payload;
    EXPECT_TRUE(EncodeGameplayCatalog(fixture.catalog, {fixture.row}, {}, &payload));
    init.AddEntry(kPackEntryKindGameplayCatalog, 0, payload);
    fixture.packs[0] = {11, init.Build()};
    fixture.RefreshManifest();
    EXPECT_FALSE(fixture.Build());
    EXPECT_TRUE(fixture.output.empty());
}

TEST(EditorWaveformPublication, WritesPortableGoldenContractToScratch) {
    Fixture fixture({{0, 0.5f, -1}, {1, -0.25f, 0}});
    ASSERT_TRUE(fixture.Build()) << fixture.error;
    ASSERT_FALSE(fixture.manifest.empty());
    ASSERT_FALSE(fixture.output.empty());
    const auto golden = std::filesystem::path(HARKEN_EDITOR_METADATA_GOLDEN_DIR) / "golden";
    for (const auto& [name, expected] : std::vector<std::pair<std::string, std::string>>{
        {"HarkenPackManifest.json", fixture.manifest}, {"HarkenEditorWaveforms.json", fixture.output}}) {
        std::ifstream file(golden / name, std::ios::binary);
        const std::string actual{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        EXPECT_EQ(actual, expected) << name;
    }
    const auto root = std::filesystem::path(HARKEN_TEMP_DIR) / "editor-waveform-v2-golden";
    std::filesystem::create_directories(root);
    std::ofstream(root / "HarkenPackManifest.json", std::ios::binary) << fixture.manifest;
    std::ofstream(root / "HarkenEditorWaveforms.json", std::ios::binary) << fixture.output;
    for (const auto& pack : fixture.packs) {
        std::ofstream file(root / (pack.packStableId == 11 ? "Init.pack" : "action_4.pack"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(pack.bytes.data()), pack.bytes.size());
        ASSERT_TRUE(file);
    }
}
} // namespace
