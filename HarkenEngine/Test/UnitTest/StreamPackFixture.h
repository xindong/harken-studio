#pragma once

#include "Common/ExternalPcmFixture.h"
#include "UnitTest/CanonicalPackSetActivate.h"
#include "harken/core/detail/CurrentWorldValidation.h"

#include <algorithm>
#include <iterator>
#include <chrono>
#include <thread>

namespace harkentest {

// Build a real Pack closure from existing playback fixtures. External metadata
// remains lazy; activation never opens or prewarms a stream.
class StreamPackFixture {
public:
    explicit StreamPackFixture(harken::Engine& engine, const char* name)
        : engine_(engine), directory_(std::filesystem::path(HARKEN_TEMP_DIR) / name) {}

    VnResult AddPcm(VnSoundId id, const VnPCMDesc& desc) {
        Row row;
        auto planar = desc;
        std::vector<float> samples;
        std::array<const float*, harken::kMaxChannels> planes{};
        if (desc.layout == VN_PCM_LAYOUT_INTERLEAVED) {
            samples.resize(desc.frames * desc.channels);
            for (std::uint32_t channel = 0; channel < desc.channels; ++channel) {
                planes[channel] = samples.data() + channel * desc.frames;
                for (std::uint64_t frame = 0; frame < desc.frames; ++frame)
                    samples[channel * desc.frames + frame] = desc.samplesInterleaved[frame * desc.channels + channel];
            }
            planar.layout = VN_PCM_LAYOUT_PLANAR;
            planar.samplesPlanar = planes.data();
        }
        planar.policy = VN_PCM_POLICY_COPY;
        const auto result = engine_.registry.PreparePackSamples(id, &planar, &row.prepared);
        if (result != VN_OK) return result;
        row.facts = row.prepared.Facts();
        row.id = id;
        rows_.push_back(std::move(row));
        return VN_OK;
    }

    VnResult AddStreamFile(VnSoundId id, const std::string& path) {
        std::ifstream input(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
        if (!input) return VN_BAD_STREAM_FILE;
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
        Row row;
        row.range = ExternalPcmFixtureRange(bytes,
            [path](auto) { return harken::OpenStreamByteSource(path.c_str()); });
        const auto result = engine_.registry.PreparePackRangeSamples(id, row.range, &row.prepared);
        if (result != VN_OK) return result;
        row.facts = row.prepared.Facts();
        row.id = id;
        rows_.push_back(std::move(row));
        return VN_OK;
    }

    void AddVoiceBinding(VnBindingId id, const VnBindingDesc& desc, VnSoundId sound) {
        harken::PackBindingAttachmentEntry attachment{};
        attachment.bindingId = id;
        attachment.scopeKind = VN_TARGET_SCOPE_VOICE;
        attachment.objectId = sound;
        bindings_.push_back({id, desc, attachment});
    }

    harken::PcmMediaFacts Facts(VnSoundId id) const {
        for (const auto& row : rows_) if (row.id == id) return row.facts;
        return {};
    }

    VnResult Activate(harken::ContainerWorldInstallViewV1 world, const harken::PlayableRefV1& root = {},
        std::span<const harken::PlayableRefV1> peers = {}) {
        using namespace harken;
        using namespace packfoundation;
        std::vector<PlayablePropertyProgramViewV1> properties;
        if (world.playablePropertyCount)
            properties.assign(world.playableProperties, world.playableProperties + world.playablePropertyCount);
        std::sort(properties.begin(), properties.end(), [](const auto& a, const auto& b) {
            return std::tie(a.contentPropertyKind, a.stableId) < std::tie(b.contentPropertyKind, b.stableId);
        });
        world.playableProperties = properties.data();
        std::vector<ActionStepProgramViewV1> steps;
        const auto appendPlay = [&](const PlayableRefV1& target) {
            ActionStepProgramViewV1 step{};
            step.opcode = ActionOpcodeV1::Play;
            step.authoredEventIndex = static_cast<std::uint32_t>(steps.size());
            switch (target.kind) {
            case PlayableKindV1::Sound: step.target.kind = ActionTargetKindV1::Sound; break;
            case PlayableKindV1::Track: step.target.kind = ActionTargetKindV1::Track; break;
            case PlayableKindV1::Segment: step.target.kind = ActionTargetKindV1::Segment; break;
            default: step.target.kind = ActionTargetKindV1::Container; break;
            }
            step.target.stableId = target.stableId;
            step.target.expectedProgramSemanticFingerprint = target.expectedProgramSemanticFingerprint;
            SetAbsoluteTime(&step.delay);
            SetAbsoluteTime(&step.fade);
            SetAbsoluteTime(&step.cooldown);
            steps.push_back(step);
        };
        if (root.stableId != VnObjectId{}) appendPlay(root);
        for (const auto& peer : peers) appendPlay(peer);
        ActionProgramViewV1 action{};
        action.actionStableId = RootAction();
        action.steps = steps.data();
        action.stepCount = action.expandedStepOccurrenceCount = static_cast<std::uint32_t>(steps.size());
        action.expandedFiringCount = 1;
        action.programSemanticFingerprint = ComputeActionProgramFingerprintV1(action);
        std::vector<ActionProgramViewV1> actions;
        if (world.actionCount) actions.assign(world.actions, world.actions + world.actionCount);
        if (!steps.empty()) actions.push_back(action);
        std::sort(actions.begin(), actions.end(), [](const auto& a, const auto& b) { return a.actionStableId < b.actionStableId; });
        world.actions = actions.data();
        world.actionCount = static_cast<std::uint32_t>(actions.size());

        ContainerWorldInstallViewV1 initWorld{};
        initWorld.capacity = world.capacity;
        initWorld.identity.projectId = world.identity.projectId;
        const auto initBytes = BuildWorld(initWorld, true);
        const auto init = ActivateInitBytesV1(directory_, initBytes, kInitPackStableId);
        if (init.result != VN_OK) return init.result;
        std::sort(rows_.begin(), rows_.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        const auto contentBytes = BuildWorld(world, false);
        PackCanonicalImage parsed;
        const auto parsedResult = ParsePackCanonicalImage(contentBytes.data(), contentBytes.size(), &parsed);
        EXPECT_EQ(VN_OK, parsedResult);
        if (parsedResult != VN_OK) return parsedResult;
        const auto path = (directory_ / "Content.pack").string();
        if (!WriteBytes(path, contentBytes)) return VN_DEVICE_ERROR;
        std::vector<PCMRegistry::PreparedPackSamples> prepared;
        for (auto& row : rows_) prepared.push_back(std::move(row.prepared));
        VnPackLoadDesc load{};
        load.structSize = sizeof(load);
        load.utf8Path = path.c_str();
        const auto result = LoadProvisionalCanonicalContentForTests(engine_, &load, &content_, &prepared);
        return result;
    }

    // Use only when the scenario assumes validated source metadata at start.
    // Initial preparation tests deliberately leave this step to normal playback.
    VnResult PrepareSources() {
        for (const auto& row : rows_) {
            if (!row.range) continue;
            harken::StreamManager::StreamFacts facts;
            auto result = engine_.streams.ResolvePackSource(row.range, &facts);
            if (result != VN_OK) return result;
            if (!engine_.streams.AcquirePendingRef(facts.fileIndex)) return VN_BACKPRESSURE;
            auto state = harken::StreamManager::PreparationState::Preparing;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (state == harken::StreamManager::PreparationState::Preparing &&
                std::chrono::steady_clock::now() < deadline) {
                state = engine_.streams.PreparePackSource(facts.fileIndex, facts.sourceGeneration,
                    engine_.vorbis, engine_.config.maxPackDecodeStagingBytes, &result);
                if (state == harken::StreamManager::PreparationState::Preparing)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            engine_.streams.ReleasePendingRef(facts.fileIndex);
            if (state != harken::StreamManager::PreparationState::Ready)
                return result == VN_OK ? VN_DEVICE_ERROR : result;
        }
        return VN_OK;
    }

    struct Start {
        VnResult result = VN_INVALID_ARG;
        VnActionFiringHandle firing = 0;
        std::uint64_t instance = 0;
        std::uint64_t ticket = 0;
    };
    Start Post(std::uint64_t emitter = 0, std::uint64_t seed = 0) {
        VnPostActionDesc post{};
        post.structSize = sizeof(post);
        post.actionId = RootAction();
        post.emitter = emitter;
        post.callerSeed = seed;
        Start out;
        out.result = Vn_PostAction(&post, &out.firing);
        if (out.result != VN_OK) return out;
        VnActionStepResult step{};
        step.structSize = sizeof(step);
        out.result = Vn_QueryActionStep(out.firing, 0, &step);
        if (out.result != VN_OK) return out;
        out.result = step.result;
        out.instance = step.producedPlayableInstance;
        out.ticket = step.logicCommitSequence;
        return out;
    }

private:
    struct Row {
        VnSoundId id{};
        harken::PcmMediaFacts facts{};
        harken::PCMRegistry::PreparedPackSamples prepared;
        std::shared_ptr<const harken::PackMediaRange> range;
    };
    static VnObjectId RootAction() { return ObjectId(0xFA000001); }
    std::vector<std::uint8_t> BuildWorld(harken::ContainerWorldInstallViewV1 world, bool init) const {
        using namespace harken;
        using namespace packfoundation;
        const auto id = init ? kInitPackStableId : kResidentPackStableId;
        const auto bus = DefaultRuntimeBusFixture();
        world.identity.capabilityMask = detail::ComputeCurrentWorldLocalCapabilityMaskV1(world);
        world.identity.runtimeSemanticFingerprint = world.sourceBodyVintage == kCurrentWorldBodyVintageV19
            ? ComputeCurrentWorldFingerprintV6(world) : ComputeCurrentWorldFingerprintV4(world);
        EXPECT_EQ(WorldInstallStatus::Ok, detail::ValidateCurrentWorldPartitionLocalV1(world));
        std::vector<std::uint8_t> body;
        if (!(world.sourceBodyVintage == kCurrentWorldBodyVintageV19
            ? EncodeCurrentWorldBodyV19(world, body) : EncodeCurrentWorldShippingBody(world, body))) return {};
        PackCanonicalPartitionHeader header{};
        header.headerBytes = kPackCanonicalPartitionHeaderBytes;
        header.bodyVintage = world.sourceBodyVintage;
        header.bodyBytes = body.size();
        header.bodyChecksum = Fnv1a64(body.data(), body.size());
        header.packStableId = id;
        header.partitionRevision = 1;
        header.ownedDefinitionCount = world.variableCount + world.playablePropertyCount + world.actionCount;
        ApplyCanonicalBusTriple(&header, bus.triple);
        std::vector<std::uint8_t> partition(header.headerBytes + body.size());
        SerializePackCanonicalPartitionHeader(header, partition.data());
        header.headerChecksum = Fnv1a64(partition.data(), kPackCanonicalPartitionHeaderHeaderChecksumOffset);
        SerializePackCanonicalPartitionHeader(header, partition.data());
        std::memcpy(partition.data() + header.headerBytes, body.data(), body.size());
        ImageBuilder image(init ? kPackKindInit : kPackKindCanonicalContent);
        image.AddEntry(kPackEntryKindPackRequirementBlock, id,
            RequirementPayload(id, 1, 1, header.ownedDefinitionCount, body.size(), bus.triple));
        image.AddEntry(kPackEntryKindCanonicalDefinitionPartition, id, std::move(partition));
        if (init) AppendRuntimeBusFixture(&image, bus);
        else {
            image.AddEntry(kPackEntryKindPackDependencyEdge, VnObjectId{},
                DependencyPayload(kInitPackStableId, 1, bus.triple), kPackPackDependencyEdgeVintage);
            // Prepared samples replace the temporary directory's PCM payloads.
            for (const auto& row : rows_)
                image.AddMedia(row.id, std::vector<float>(row.facts.frames * row.facts.channelCount),
                    row.facts.loop, static_cast<std::uint16_t>(row.facts.channelCount));
        }
        if (!init) for (std::size_t index = 0; index < bindings_.size(); ++index) {
            const auto& binding = bindings_[index];
            std::vector<std::uint8_t> definition(kPackBindingDefinitionBytes);
            WritePackBindingDefinition(binding.id, binding.desc, definition.data());
            image.AddEntry(kPackEntryKindBindingDefinition, binding.id, std::move(definition),
                kPackBindingDefinitionVintage);
            std::vector<std::uint8_t> attachment(kPackBindingAttachmentBytes);
            SerializePackBindingAttachment(binding.attachment, attachment.data());
            const auto entry = image.AddEntry(kPackEntryKindBindingAttachment, VnObjectId{},
                std::move(attachment), kPackBindingAttachmentVintage);
            image.SetRelationOrdinal(entry, index);
        }
        return image.Build();
    }
    struct Binding {
        VnBindingId id;
        VnBindingDesc desc;
        harken::PackBindingAttachmentEntry attachment;
    };
    std::vector<Binding> bindings_;
    harken::Engine& engine_;
    std::filesystem::path directory_;
    std::vector<Row> rows_;
    VnPackHandle content_ = 0;
};

} // namespace harkentest
