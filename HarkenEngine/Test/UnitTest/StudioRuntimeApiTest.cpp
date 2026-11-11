#include <harken/Harken.h>
#include "Common/ObjectIdTestSupport.h"
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include <harken_preview/PreviewProject.h>
#include <harken_current_world_compiler/CurrentWorldCompiler.h>
#include <harken_bus_compiler/BusCompiler.h>
#include <harken_studio_build/WorldCodec.h>
#include <harken/core/ChannelLayout.h>
#include <harken/media/PackLogicImage.h>
#include <harken/core/PreviewBusEffects.h>

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <vector>

namespace {

class StudioRuntimeApi : public testing::Test {
protected:
    void SetUp() override {
        config.structSize = sizeof(config);
        config.deviceMode = VN_DEVICE_NULL;
        config.sampleRate = 48000;
        config.blockFrames = 128;
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1({}, {}, {}, {});
        ASSERT_TRUE(compiled.ok());
        ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(compiled.world->View(), body));
        desc.structSize = sizeof(desc);
        desc.bodyVintage = harkenstudiobuild::kCurrentWorldShippingBodyVintage;
        desc.projectSessionId = 1;
        desc.authoringRevision = 1;
        desc.runtimeEpoch = 1;
        desc.submissionSerial = 1;
        desc.publishedBusFingerprint = 1;
        desc.compileTargetProfileFingerprint = 1;
        desc.ownerId = 1;
        desc.ownerRevision = 1;
        desc.body = body.data();
        desc.bodyBytes = body.size();
        desc.buses = &master;
        desc.busCount = 1;
        v2Buses = {{{harkentest::ObjectId(2), VN_BUS_MASTER},
                    {VN_BUS_MASTER, VnBusId{}}}};
        v2Volumes = {{{harkentest::ObjectId(2), -6.0f, 0.0f},
                      {VN_BUS_MASTER, 0.0f, 0.0f}}};
    }

    VnPreviewProjectDescV2 VolumeDesc(std::uint32_t capability = 3) {
        VnPreviewProjectDescV2 v2{};
        v2.base = desc;
        v2.base.structSize = sizeof(v2);
        v2.base.buses = v2Buses.data();
        v2.base.busCount = static_cast<std::uint32_t>(v2Buses.size());
        v2.busVolumes = v2Volumes.data();
        v2.busVolumeCount = static_cast<std::uint32_t>(v2Volumes.size());
        v2.profileVintage = 1;
        v2.renderSampleRateHz = 48000;
        v2.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
        v2.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
        v2.runtimeCapabilityMask = harken::busfingerprint::kRequiredBusCapabilityMaskV1;
        v2.base.compileTargetProfileFingerprint = harken::busfingerprint::ProfileFingerprintV1(
            v2.profileVintage, v2.renderSampleRateHz, v2.mixLayout,
            v2.deviceLayout, v2.runtimeCapabilityMask);
        image = {};
        image.hasBusDefinitionSnapshot = true;
        auto& header = image.busDefinitionSnapshotHeader;
        header.schemaVersion = 2;
        header.storageModelVersion = 2;
        header.publicationCapabilityVintage = capability;
        header.profileVintage = v2.profileVintage;
        header.renderSampleRateHz = v2.renderSampleRateHz;
        header.mixLayout = v2.mixLayout;
        header.deviceLayout = v2.deviceLayout;
        header.runtimeCapabilityMask = v2.runtimeCapabilityMask;
        header.compileTargetProfileFingerprint = v2.base.compileTargetProfileFingerprint;
        header.busCount = 2;
        header.downmixCount = 2;
        header.coefficientCount = 128;
        for (std::uint32_t i = 0; i < 2; ++i) {
            const auto& bus = v2Buses[i];
            const auto& volume = v2Volumes[i];
            image.busDefinitionBusesV2.push_back({
                .busId = bus.id,
                .parentId = bus.parent,
                .feedsFirstOrdinal = i,
                .faderDb = volume.volumeDb,
                .outputBusVolumeDb = volume.outputBusVolumeDb,
                .faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1,
                .outputLayout = VN_CHANNEL_LAYOUT_STEREO,
                .downmixFirst = i,
                .downmixCount = 1});
            image.busDefinitionDownmixesV2.push_back({bus.id, VN_CHANNEL_LAYOUT_STEREO,
                VN_CHANNEL_LAYOUT_STEREO, i * 64u, 64});
            std::array<float, 64> coefficients{};
            harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO,
                VN_CHANNEL_LAYOUT_STEREO, coefficients.data());
            image.busDefinitionCoefficients.insert(image.busDefinitionCoefficients.end(),
                coefficients.begin(), coefficients.end());
        }
        v2.base.publishedBusFingerprint = harken::CanonicalVolumeBusFingerprint(image);
        return v2;
    }

    void TearDown() override {
        VnPreview_CancelProject(handle);
        EXPECT_EQ(VN_OK, Vn_Term());
    }

    VnPreviewProjectDescV4 EffectDesc(float ceilingDb, std::uint32_t flags) {
        auto volume = VolumeDesc();
        volume.runtimeCapabilityMask |= (UINT64_C(1) << 19) |
            (UINT64_C(1) << (32 + VN_EFFECT_LIMITER));
        volume.base.compileTargetProfileFingerprint = harken::busfingerprint::ProfileFingerprintV1(
            volume.profileVintage, volume.renderSampleRateHz, volume.mixLayout,
            volume.deviceLayout, volume.runtimeCapabilityMask);
        effectParameters = {{{0, ceilingDb}, {1, 100.0f}}};
        effect = {v2Buses[0].id, harkentest::ObjectId(90), 0, VN_EFFECT_LIMITER, 2, flags, 0, 2};
        harkenbuscompiler::CompiledBusResultV1 compiled;
        compiled.compileTargetProfile = harkenprojectschema::MakeCompileTargetProfileV1(
            volume.profileVintage, volume.renderSampleRateHz, volume.mixLayout,
            volume.deviceLayout, volume.runtimeCapabilityMask);
        compiled.compileTargetProfileFingerprint = volume.base.compileTargetProfileFingerprint;
        compiled.buses.push_back({
            .busId = harkentest::ToStudio(v2Buses[0].id),
            .parentId = harkenstudio::kMasterBusObjectId,
            .faderDb = v2Volumes[0].volumeDb,
            .faderAfterSlot = 8,
            .effectCount = 1,
            .outputBusVolumeDb = v2Volumes[0].outputBusVolumeDb});
        compiled.effects.push_back({harkentest::ToStudio(effect.busId), harkentest::ToStudio(effect.effectId),
            effect.slot, effect.effectType, effect.descriptorVintage, effect.flags, 0, 2});
        for (const auto& parameter : effectParameters)
            compiled.effectParameters.push_back({parameter.index, parameter.value});
        const auto projection = harkenbuscompiler::BuildPublishedBusProjectionV1(compiled,
            harkenbuscompiler::BusPublicationCapabilityVintageV1::TopologyVolumeSendAndEffectsNeutralOtherProperties);
        volume.base.publishedBusFingerprint = projection.fingerprint;
        VnPreviewProjectDescV4 result{};
        result.base.base = volume;
        result.base.base.base.structSize = sizeof(result);
        result.busEffects = &effect;
        result.busEffectCount = 1;
        result.busEffectParameters = effectParameters.data();
        result.busEffectParameterCount = static_cast<std::uint32_t>(effectParameters.size());
        result.busFaderAfterSlots = faderSlots.data();
        result.busFaderCount = static_cast<std::uint32_t>(faderSlots.size());
        return result;
    }

    harken::PackCanonicalImage image;
    VnConfig config{};
    VnPreviewBusV1 master{VN_BUS_MASTER, VN_BUS_MASTER};
    std::array<VnPreviewBusV1, 2> v2Buses{};
    std::array<VnPreviewBusVolumeV2, 2> v2Volumes{};
    VnPreviewProjectDescV1 desc{};
    VnPreviewProjectHandle handle = 0;
    std::vector<std::uint8_t> body;
    VnPreviewBusEffectV1 effect{};
    std::array<VnPreviewBusEffectParameterV1, 2> effectParameters{};
    std::array<std::uint32_t, 2> faderSlots{8, 8};
};

#if HARKEN_TEST_PREVIEW_ENABLED
TEST_F(StudioRuntimeApi, PreparedTransactionOwnsInputAndCommitsOnlyOnce) {
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    ASSERT_NE(0u, handle);
    std::fill(body.begin(), body.end(), 0);
    master = {};
    std::uint64_t generation = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    ASSERT_NE(0u, generation);
    for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(generation); ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        VnPreview_PumpNullMixer(1);
        VnPreview_DrainProjectAcks();
    }
    EXPECT_TRUE(VnPreview_ProjectIsOpen(generation));
    std::uint64_t revision = 0;
    EXPECT_TRUE(VnPreview_QueryOwnerRevision(desc.ownerId, &revision));
    EXPECT_EQ(desc.ownerRevision, revision);
    EXPECT_NE(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    EXPECT_EQ(0u, generation);
}

TEST_F(StudioRuntimeApi, CancelReleasesReservationsAndOldHandlesStayInvalid) {
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
        VnPreview_CancelProject(handle);
        std::uint64_t generation = 99;
        EXPECT_NE(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
        EXPECT_EQ(0u, generation);
    }
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    ASSERT_EQ(VN_OK, Vn_Term());
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    std::uint64_t generation = 99;
    EXPECT_NE(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    EXPECT_EQ(0u, generation);
}

TEST_F(StudioRuntimeApi, ConcurrentBusChangeDoesNotPartiallyPublishPreparedWorld) {
    auto v2 = VolumeDesc(4);
    std::array<VnPreviewBusV1, 3> buses{{
        {harkentest::ObjectId(2), VN_BUS_MASTER},
        {harkentest::ObjectId(3), VN_BUS_MASTER},
        {VN_BUS_MASTER, VnBusId{}}}};
    std::array<VnPreviewBusVolumeV2, 3> volumes{{
        {buses[0].id, -6.0f, 0.0f},
        {buses[1].id, 0.0f, 0.0f},
        {VN_BUS_MASTER, 0.0f, 0.0f}}};
    const VnPreviewBusSendV1 send{buses[0].id, buses[1].id};
    float sendDb = -6.0f;
    v2.base.buses = buses.data();
    v2.base.busCount = static_cast<std::uint32_t>(buses.size());
    v2.base.sends = &send;
    v2.base.sendCount = 1;
    v2.busVolumes = volumes.data();
    v2.busVolumeCount = static_cast<std::uint32_t>(volumes.size());
    harken::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = 2;
    header.storageModelVersion = 2;
    header.publicationCapabilityVintage = 4;
    header.profileVintage = v2.profileVintage;
    header.renderSampleRateHz = v2.renderSampleRateHz;
    header.mixLayout = v2.mixLayout;
    header.deviceLayout = v2.deviceLayout;
    header.runtimeCapabilityMask = v2.runtimeCapabilityMask;
    header.compileTargetProfileFingerprint = v2.base.compileTargetProfileFingerprint;
    header.busCount = 3;
    header.sendCount = 1;
    header.downmixCount = 3;
    header.coefficientCount = 192;
    for (std::uint32_t i = 0; i < 3; ++i) {
        image.busDefinitionBusesV2.push_back({
            .busId = buses[i].id,
            .parentId = buses[i].parent,
            .feedsFirstOrdinal = i,
            .faderDb = volumes[i].volumeDb,
            .outputBusVolumeDb = volumes[i].outputBusVolumeDb,
            .faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1,
            .outputLayout = VN_CHANNEL_LAYOUT_STEREO,
            .downmixFirst = i,
            .downmixCount = 1});
        image.busDefinitionDownmixesV2.push_back({buses[i].id,
            VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO, i * 64u, 64});
        std::array<float, 64> coefficients{};
        harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO,
            VN_CHANNEL_LAYOUT_STEREO, coefficients.data());
        image.busDefinitionCoefficients.insert(image.busDefinitionCoefficients.end(),
            coefficients.begin(), coefficients.end());
    }
    image.busDefinitionSendsV2.push_back({send.source, send.target, sendDb});
    v2.base.publishedBusFingerprint = harken::CanonicalVolumeBusFingerprint(image);
    VnPreviewProjectDescV3 v3{};
    v3.base = v2;
    v3.base.base.structSize = sizeof(v3);
    v3.busSendLevelsDb = &sendDb;
    v3.busSendLevelCount = 1;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV3(&v3, &handle));
    std::uint64_t firstGeneration = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &firstGeneration));
    handle = 0;
    for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(firstGeneration); ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        VnPreview_PumpNullMixer(1);
        VnPreview_DrainProjectAcks();
    }
    ASSERT_TRUE(VnPreview_ProjectIsOpen(firstGeneration));
    std::uint64_t ownerRevision = 0;
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(v3.base.base.ownerId, &ownerRevision));
    ASSERT_EQ(1u, ownerRevision);
    std::vector<float> dc(48000, 0.25f);
    const auto pcm = harkentest::MonoDesc(dc, 48000);
    std::uint64_t contentRevision = 0;
    const auto soundId = harkentest::ObjectId(90);
    ASSERT_EQ(VN_OK, VnPreview_RegisterPcm(soundId, &pcm, 0, 0, &contentRevision));
    auto play = harkentest::MakePlayDesc(soundId);
    play.busId = send.source;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, VnPreview_PlayVoiceRaw(&play, &voice));
    ASSERT_NE(0u, voice);
    ASSERT_EQ(VN_OK, Vn_Update());
    VnPreview_PumpNullMixer(1);
    VnSnapshot before{};
    before.structSize = sizeof(before);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&before));
    ASSERT_GT(before.busPeaks[0], 0.1f);

    sendDb = -12.0f;
    image.busDefinitionSendsV2[0].levelDb = sendDb;
    v3.base.base.publishedBusFingerprint = harken::CanonicalVolumeBusFingerprint(image);
    v3.base.base.ownerBaseRevision = 1;
    v3.base.base.ownerRevision = 2;
    v3.base.base.authoringRevision = 2;
    v3.base.base.submissionSerial = 2;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV3(&v3, &handle));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(91), VN_BUS_MASTER));
    VnPreview_PumpNullMixer(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    std::uint64_t refusedGeneration = 99;
    EXPECT_NE(VN_PREVIEW_COMMIT_OK,
        VnPreview_CommitProject(handle, &refusedGeneration));
    EXPECT_EQ(0u, refusedGeneration);
    VnPreview_CancelProject(handle);
    handle = 0;
    EXPECT_TRUE(VnPreview_ProjectIsOpen(firstGeneration));
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(v3.base.base.ownerId, &ownerRevision));
    EXPECT_EQ(1u, ownerRevision);
    ASSERT_EQ(VN_OK, Vn_Update());
    VnPreview_PumpNullMixer(1);
    VnSnapshot afterRefusal{};
    afterRefusal.structSize = sizeof(afterRefusal);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&afterRefusal));
    ASSERT_GT(afterRefusal.blockIndex, before.blockIndex);
    ASSERT_NEAR(before.busPeaks[0], afterRefusal.busPeaks[0], 1e-4f);
}

TEST_F(StudioRuntimeApi, ForcedPreviewWorldReplacementPreservesExactOwnerRetry) {
    auto view = EffectDesc(-12.0f, 0);
    view.forceWorldReplacement = 1;
    const auto publish = [&](std::uint64_t& generation) {
        ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&view, &handle));
        ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
        handle = 0;
        for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(generation); ++i) {
            ASSERT_EQ(VN_OK, Vn_Update());
            VnPreview_PumpNullMixer(1);
            VnPreview_DrainProjectAcks();
        }
        ASSERT_TRUE(VnPreview_ProjectIsOpen(generation));
    };
    std::uint64_t initial = 0;
    publish(initial);
    auto& base = view.base.base.base;
    base.ownerBaseRevision = 1;
    base.ownerRevision = 2;
    base.authoringRevision = 2;
    base.submissionSerial = 2;
    std::uint64_t replaced = 0;
    publish(replaced);
    EXPECT_GT(replaced, initial);
    std::uint64_t retried = 0;
    publish(retried);
    EXPECT_EQ(retried, replaced);
    std::uint64_t revision = 0;
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(base.ownerId, &revision));
    EXPECT_EQ(revision, 2u);
}

TEST_F(StudioRuntimeApi, EffectPublicationUpdatesAudioAndRetainsOwnedStateAcrossRevisions) {
    v2Volumes[0].volumeDb = 0.0f;
    std::uint64_t revision = 0;
    const auto publish = [&](float ceilingDb, std::uint32_t flags) {
        auto view = EffectDesc(ceilingDb, flags);
        auto& base = view.base.base.base;
        base.ownerBaseRevision = revision;
        base.ownerRevision = revision + 1;
        base.authoringRevision = revision + 1;
        base.submissionSerial = revision + 1;
        ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&view, &handle));
        std::uint64_t generation = 0;
        ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
        handle = 0;
        for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(generation); ++i) {
            ASSERT_EQ(VN_OK, Vn_Update());
            VnPreview_PumpNullMixer(1);
            VnPreview_DrainProjectAcks();
        }
        ASSERT_TRUE(VnPreview_ProjectIsOpen(generation));
        ++revision;
    };
    const auto peak = [&] {
        for (int i = 0; i < 64; ++i) {
            EXPECT_EQ(VN_OK, Vn_Update());
            VnPreview_PumpNullMixer(1);
        }
        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(snapshot);
        EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        return snapshot.busPeaks[0];
    };
    publish(-12.0f, 0);
    std::vector<float> dc(480000, 0.8f);
    const auto pcm = harkentest::MonoDesc(dc, 48000);
    std::uint64_t contentRevision = 0;
    const auto sound = harkentest::ObjectId(91);
    ASSERT_EQ(VN_OK, VnPreview_RegisterPcm(sound, &pcm, 0, 0, &contentRevision));
    auto play = harkentest::MakePlayDesc(sound);
    play.busId = v2Buses[0].id;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, VnPreview_PlayVoiceRaw(&play, &voice));
    const float limited = peak();
    ASSERT_GT(limited, 0.1f);
    EXPECT_LT(limited, 0.3f);
    std::array<float, 2> liveParameters{-18.0f, 100.0f};
    EXPECT_EQ(VN_IN_USE, VnPreview_SetBusEffect(desc.ownerId + 1, effect.busId,
        effect.effectId, effect.effectType, liveParameters.data(), liveParameters.size(), 0));
    EXPECT_EQ(VN_UNKNOWN_ID, VnPreview_SetBusEffect(desc.ownerId, effect.busId,
        harkentest::ObjectId(999), effect.effectType, liveParameters.data(), liveParameters.size(), 0));
    EXPECT_NEAR(limited, peak(), 0.01f);
    ASSERT_EQ(VN_OK, VnPreview_SetBusEffect(desc.ownerId, effect.busId,
        effect.effectId, effect.effectType, liveParameters.data(), liveParameters.size(), 0));
    EXPECT_NEAR(limited * 0.5f, peak(), 0.01f);
    ASSERT_EQ(VN_OK, VnPreview_SetBusEffect(desc.ownerId, effect.busId,
        effect.effectId, effect.effectType, liveParameters.data(), liveParameters.size(), 1));
    EXPECT_NEAR(0.8f, peak(), 0.01f);
    publish(-6.0f, 0);
    const float raised = peak();
    EXPECT_GT(raised, limited * 1.4f);
    EXPECT_LT(raised, 0.55f);
    publish(-6.0f, 1);
    EXPECT_NEAR(0.8f, peak(), 0.01f);
    v2Volumes[0].volumeDb = -6.0f;
    publish(-6.0f, 1);
    EXPECT_NEAR(0.8f * std::pow(10.0f, -6.0f / 20.0f), peak(), 0.01f);
    publish(-6.0f, 0);
    const float retained = peak();
    EXPECT_GT(retained, 0.1f);
    EXPECT_LT(retained, 0.3f);
    auto foreign = EffectDesc(-12.0f, 0);
    foreign.base.base.base.ownerId = 2;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&foreign, &handle));
    EXPECT_EQ(0u, handle);
    EXPECT_NEAR(retained, peak(), 0.01f);
    auto pending = EffectDesc(-18.0f, 0);
    auto& pendingBase = pending.base.base.base;
    pendingBase.ownerBaseRevision = revision;
    pendingBase.ownerRevision = revision + 1;
    pendingBase.authoringRevision = revision + 1;
    pendingBase.submissionSerial = revision + 1;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&pending, &handle));
    const VnLimiterDesc external{sizeof(VnLimiterDesc), 0, harkentest::ObjectId(92),
        -24.0f, 100.0f};
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(v2Buses[0].id, 1, VN_EFFECT_LIMITER,
        reinterpret_cast<const VnEffectDesc*>(&external)));
    const float externalPeak = peak();
    ASSERT_LT(externalPeak, retained * 0.25f);
    std::uint64_t refusedGeneration = 99;
    EXPECT_EQ(VN_PREVIEW_COMMIT_BACKPRESSURE,
        VnPreview_CommitProject(handle, &refusedGeneration));
    EXPECT_EQ(0u, refusedGeneration);
    std::uint64_t ownerRevision = 0;
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(pendingBase.ownerId, &ownerRevision));
    EXPECT_EQ(revision, ownerRevision);
    EXPECT_NEAR(externalPeak, peak(), 0.01f);
    VnPreview_CancelProject(handle);
    handle = 0;
    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(v2Buses[0].id, 1));
    EXPECT_NEAR(retained, peak(), 0.01f);
}

TEST_F(StudioRuntimeApi, OversizedPublicationRefusesWithoutBlockingLaterTransactions) {
    ASSERT_EQ(VN_OK, Vn_Term());
    config.maxControlCommandsPerQuantum = 2;
    config.commandQueueDepth = 256;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    const auto drain = [&](std::uint64_t generation) {
        for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(generation); ++i) {
            ASSERT_EQ(VN_OK, Vn_Update());
            VnPreview_PumpNullMixer(1);
            VnPreview_DrainProjectAcks();
        }
        EXPECT_TRUE(VnPreview_ProjectIsOpen(generation));
    };
    auto initial = VolumeDesc();
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV2(&initial, &handle));
    std::uint64_t generation = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    handle = 0;
    drain(generation);
    auto oversized = EffectDesc(-12.0f, 0);
    auto& requested = oversized.base.base.base;
    requested.ownerBaseRevision = 1;
    requested.ownerRevision = 2;
    requested.authoringRevision = 2;
    requested.submissionSerial = 2;
    EXPECT_EQ(VN_PREVIEW_PREPARE_CAPACITY,
        VnPreview_PrepareProjectV4(&oversized, &handle));
    EXPECT_EQ(0u, handle);
    std::uint64_t revision = 0;
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(desc.ownerId, &revision));
    EXPECT_EQ(1u, revision);
    EXPECT_TRUE(VnPreview_ProjectIsOpen(generation));
    v2Volumes[0].volumeDb = -12.0f;
    auto next = VolumeDesc();
    next.base.ownerBaseRevision = 1;
    next.base.ownerRevision = 2;
    next.base.authoringRevision = 2;
    next.base.submissionSerial = 3;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV2(&next, &handle));
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    handle = 0;
    drain(generation);
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(desc.ownerId, &revision));
    EXPECT_EQ(2u, revision);
}

TEST_F(StudioRuntimeApi, EffectPublicationValidatesCompleteInputBeforeReservingResources) {
    auto view = EffectDesc(-6.0f, 0);
    --view.busEffectParameterCount;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&view, &handle));
    EXPECT_EQ(0u, handle);
    view = EffectDesc(-6.0f, 0);
    effect.flags = 2;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&view, &handle));
    view = EffectDesc(-6.0f, 0);
    effectParameters[0].value = -12.0f;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&view, &handle));
    view = EffectDesc(-6.0f, 0);
    for (int i = 0; i < 32; ++i) {
        ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&view, &handle));
        VnPreview_CancelProject(handle);
        handle = 0;
    }
}

TEST_F(StudioRuntimeApi, EffectPublicationFollowsTopologyOrderAcrossEmptyBuses) {
    auto view = EffectDesc(-6.0f, 0);
    const std::array<VnPreviewBusV1, 4> buses{{
        {harkentest::ObjectId(9), harkentest::ObjectId(3)},
        {harkentest::ObjectId(3), harkentest::ObjectId(2)},
        {harkentest::ObjectId(2), VN_BUS_MASTER},
        {VN_BUS_MASTER, VnBusId{}}}};
    const std::array<VnPreviewBusVolumeV2, 4> volumes{{
        {buses[0].id, 0.0f, 0.0f}, {buses[1].id, 0.0f, 0.0f},
        {buses[2].id, 0.0f, 0.0f}, {buses[3].id, 0.0f, 0.0f}}};
    const std::array<std::uint32_t, 4> faders{8, 8, 8, 8};
    const std::array<VnPreviewBusEffectV1, 2> effects{{
        {buses[0].id, harkentest::ObjectId(90), 0, VN_EFFECT_LIMITER, 2, 0, 0, 2},
        {buses[1].id, harkentest::ObjectId(91), 0, VN_EFFECT_LIMITER, 2, 0, 2, 2}}};
    const std::array<VnPreviewBusEffectParameterV1, 4> parameters{{
        {0, -6.0f}, {1, 100.0f}, {0, -12.0f}, {1, 100.0f}}};
    harkenbuscompiler::CompiledBusResultV1 compiled;
    const auto& profile = view.base.base;
    compiled.compileTargetProfile = harkenprojectschema::MakeCompileTargetProfileV1(
        profile.profileVintage, profile.renderSampleRateHz, profile.mixLayout,
        profile.deviceLayout, profile.runtimeCapabilityMask);
    compiled.compileTargetProfileFingerprint = profile.base.compileTargetProfileFingerprint;
    for (std::uint32_t i = 0; i < 3; ++i)
        compiled.buses.push_back({
            .busId = harkentest::ToStudio(buses[i].id),
            .parentId = harkentest::ToStudio(buses[i].parent),
            .feedsFirstOrdinal = i,
            .faderAfterSlot = 8,
            .effectFirst = std::min(i, 2u),
            .effectCount = i < 2 ? 1u : 0u});
    for (const auto& row : effects)
        compiled.effects.push_back({harkentest::ToStudio(row.busId),
            harkentest::ToStudio(row.effectId), row.slot, row.effectType,
            row.descriptorVintage, row.flags, row.parameterFirst, row.parameterCount});
    for (const auto& row : parameters)
        compiled.effectParameters.push_back({row.index, row.value});
    const auto projection = harkenbuscompiler::BuildPublishedBusProjectionV1(compiled,
        harkenbuscompiler::BusPublicationCapabilityVintageV1::TopologyVolumeSendAndEffectsNeutralOtherProperties);
    view.base.base.base.buses = buses.data();
    view.base.base.base.busCount = static_cast<std::uint32_t>(buses.size());
    view.base.base.base.publishedBusFingerprint = projection.fingerprint;
    view.base.base.busVolumes = volumes.data();
    view.base.base.busVolumeCount = static_cast<std::uint32_t>(volumes.size());
    view.busFaderAfterSlots = faders.data();
    view.busFaderCount = static_cast<std::uint32_t>(faders.size());
    view.busEffects = effects.data();
    view.busEffectCount = static_cast<std::uint32_t>(effects.size());
    view.busEffectParameters = parameters.data();
    view.busEffectParameterCount = static_cast<std::uint32_t>(parameters.size());
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&view, &handle));
    std::uint64_t generation = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(generation); ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        VnPreview_PumpNullMixer(1);
        VnPreview_DrainProjectAcks();
    }
    EXPECT_TRUE(VnPreview_ProjectIsOpen(generation));
}


TEST_F(StudioRuntimeApi, V4CopiesTypedEffectsAndRejectsInvalidOrUnfingerprintedRows) {
    VnPreviewProjectDescV4 v4{};
    v4.base.base = VolumeDesc(5);
    v4.base.base.base.structSize = sizeof(v4);
    VnPreviewBusEffectV1 row{v2Buses[0].id, harkentest::ObjectId(100), 0,
        VN_EFFECT_LIMITER, 1, 0, 0, 2};
    std::array<VnPreviewBusEffectParameterV1, 2> params{{{0, -12}, {1, 100}}};
    std::array<std::uint32_t, 2> positions{8, 8};
    v4.busEffects = &row;
    v4.busEffectCount = 1;
    v4.busEffectParameters = params.data();
    v4.busEffectParameterCount = 2;
    v4.busFaderAfterSlots = positions.data();
    v4.busFaderCount = 2;
    image.busDefinitionSnapshotHeader.effectCount = 1;
    image.busDefinitionSnapshotHeader.effectParameterCount = 2;
    image.busDefinitionBusesV2[0].effectCount = 1;
    image.busDefinitionBusesV2[1].effectFirst = 1;
    std::array<harken::PreviewBusEffect, 1> rows{{{row.busId, row.effectId, row.slot,
        row.effectType, row.descriptorVintage, row.flags, 0, 2}}};
    std::array<harken::PreviewEffectParameter, 2> values{{{0, -12}, {1, 100}}};
    v4.base.base.base.publishedBusFingerprint = harken::CanonicalVolumeBusFingerprint(image, rows, values);
    row.flags = 2;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV4(&v4, &handle));
    row.flags = 0;
    params[0].value = 1;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV4(&v4, &handle));
    params[0].value = -6;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV4(&v4, &handle));
    params[0].value = -12;
    positions[0] = 7;
    EXPECT_EQ(VN_PREVIEW_PREPARE_UNSUPPORTED, VnPreview_PrepareProjectV4(&v4, &handle));
    positions[0] = 8;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&v4, &handle));
    row = {};
    params = {};
    positions = {};
    std::uint64_t generation = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    VnPreview_PumpNullMixer(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_SetBusEffectParam(v2Buses[0].id, 0, 0, -6));
    EXPECT_EQ(VN_OK, Vn_SetBusEffectBypass(v2Buses[0].id, 0, 1));
    EXPECT_EQ(VN_IN_USE, Vn_ClearBusEffect(v2Buses[0].id, 0));
}


TEST_F(StudioRuntimeApi, V4AcceptsMasterOnlyEffectsAndRejectsZeroBuses) {
    VnPreviewProjectDescV4 v4{};
    v4.base.base = VolumeDesc(5);
    v4.base.base.base.structSize = sizeof(v4);
    v4.base.base.base.buses = &v2Buses[1];
    v4.base.base.base.busCount = 1;
    v4.base.base.busVolumes = &v2Volumes[1];
    v4.base.base.busVolumeCount = 1;
    VnPreviewBusEffectV1 row{VN_BUS_MASTER, harkentest::ObjectId(100), 0,
        VN_EFFECT_LIMITER, 1, 0, 0, 2};
    std::array<VnPreviewBusEffectParameterV1, 2> params{{{0, -12}, {1, 100}}};
    std::uint32_t position = 8;
    v4.busEffects = &row;
    v4.busEffectCount = 1;
    v4.busEffectParameters = params.data();
    v4.busEffectParameterCount = 2;
    v4.busFaderAfterSlots = &position;
    v4.busFaderCount = 1;
    image.busDefinitionSnapshotHeader.busCount = 1;
    image.busDefinitionSnapshotHeader.effectCount = 1;
    image.busDefinitionSnapshotHeader.effectParameterCount = 2;
    image.busDefinitionSnapshotHeader.downmixCount = 1;
    image.busDefinitionSnapshotHeader.coefficientCount = 64;
    image.busDefinitionBusesV2.erase(image.busDefinitionBusesV2.begin());
    image.busDefinitionBusesV2[0].feedsFirstOrdinal = 0;
    image.busDefinitionBusesV2[0].downmixFirst = 0;
    image.busDefinitionBusesV2[0].effectCount = 1;
    image.busDefinitionDownmixesV2.erase(image.busDefinitionDownmixesV2.begin());
    image.busDefinitionDownmixesV2[0].coefficientFirst = 0;
    image.busDefinitionCoefficients.resize(64);
    std::array<harken::PreviewBusEffect, 1> rows{{{row.busId, row.effectId, row.slot,
        row.effectType, row.descriptorVintage, row.flags, 0, 2}}};
    std::array<harken::PreviewEffectParameter, 2> values{{{0, -12}, {1, 100}}};
    v4.base.base.base.publishedBusFingerprint = harken::CanonicalVolumeBusFingerprint(image, rows, values);
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV4(&v4, &handle));
    std::uint64_t generation = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    VnPreview_PumpNullMixer(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_SetBusEffectBypass(VN_BUS_MASTER, 0, 1));
    v4.base.base.base.busCount = 0;
    v4.base.base.busVolumeCount = 0;
    v4.busFaderCount = 0;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV4(&v4, &handle));
}

TEST_F(StudioRuntimeApi, InvalidDescriptorsDoNotReserveAHandle) {
    handle = 99;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(nullptr, &handle));
    EXPECT_EQ(0u, handle);
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, nullptr));
    --desc.structSize;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    ++desc.structSize;
    const auto size = desc.bodyBytes;
    desc.bodyBytes = 1;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    desc.bodyBytes = size;
    std::array<VnPreviewBusV1, 2> duplicate{master, master};
    desc.buses = duplicate.data();
    desc.busCount = 2;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    EXPECT_EQ(0u, handle);
    duplicate[1] = {harkentest::ObjectId(9), VN_BUS_MASTER};
    const std::array<VnPreviewBusSendV1, 2> sends{{
        {duplicate[1].id, VN_BUS_MASTER}, {duplicate[1].id, VN_BUS_MASTER}}};
    desc.sends = sends.data();
    desc.sendCount = 2;
    EXPECT_NE(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    EXPECT_EQ(0u, handle);
    desc.sends = nullptr;
    desc.sendCount = 0;
    desc.buses = &master;
    desc.busCount = 1;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
}
#else
TEST_F(StudioRuntimeApi, DisabledPreviewCannotPrepareLegacyOrTypedEffects) {
    handle = 99;
    EXPECT_EQ(VN_PREVIEW_PREPARE_UNAVAILABLE, VnPreview_PrepareProject(&desc, &handle));
    EXPECT_EQ(0u, handle);
    VnPreviewProjectDescV4 v4{};
    v4.base.base = VolumeDesc(5);
    v4.base.base.base.structSize = sizeof(v4);
    std::array<std::uint32_t, 2> positions{8, 8};
    v4.busFaderAfterSlots = positions.data();
    v4.busFaderCount = 2;
    handle = 99;
    EXPECT_EQ(VN_PREVIEW_PREPARE_UNAVAILABLE, VnPreview_PrepareProjectV4(&v4, &handle));
    EXPECT_EQ(0u, handle);
}
#endif
}
