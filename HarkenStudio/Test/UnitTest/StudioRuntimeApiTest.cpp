#include <harken/Harken.h>
#include "UnitTest/ObjectIdTestSupport.h"
#include "UnitTest/TestUtil.h"
#include <harken_preview/PreviewProject.h>
#include <harken_current_world_compiler/CurrentWorldCompiler.h>
#include <harken_studio_build/WorldCodec.h>
#include <harken_studio_build/MediaContentRevision.h>
#include <harken/core/ChannelLayout.h>
#include <harken/media/PackLogicImage.h>

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#if defined(HARKEN_TEST_RUNTIME_LIBRARY)
#include <dlfcn.h>
#endif

namespace {

#if defined(HARKEN_TEST_RUNTIME_LIBRARY)
TEST(StudioRuntimeExports, PublicApiHidesBackendEntryPoints) {
    auto* library = dlopen(HARKEN_TEST_RUNTIME_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(nullptr, library) << dlerror();
    for (const auto* symbol : {"Vn_Init", "Vn_Term", "VnPreview_PlayPlayable",
                              "VnPreview_PrepareProject"}) {
        EXPECT_NE(nullptr, dlsym(library, symbol)) << symbol;
    }
    for (const auto* symbol : {"ma_context_init", "ma_device_init", "ma_decoder_init"}) {
        EXPECT_EQ(nullptr, dlsym(library, symbol)) << symbol;
    }
    EXPECT_EQ(0, dlclose(library));
}
#endif

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
        v2Volumes = {{{harkentest::ObjectId(2), -6.0f, 3.0f, 0.0f, 0},
                      {VN_BUS_MASTER, 0.0f, 0.0f, 0.0f, 0}}};
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
        harken::PackCanonicalImage image;
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
            image.busDefinitionBusesV2.push_back({bus.id, bus.parent, i,
                volume.volumeDb, volume.makeUpGainDb, volume.outputBusVolumeDb,
                harken::busfingerprint::kNeutralFaderAfterSlotV1,
                VN_CHANNEL_LAYOUT_STEREO, 0, 0, i, 1});
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

    VnConfig config{};
    VnPreviewBusV1 master{VN_BUS_MASTER, VN_BUS_MASTER};
    std::array<VnPreviewBusV1, 2> v2Buses{};
    std::array<VnPreviewBusVolumeV2, 2> v2Volumes{};
    VnPreviewProjectDescV1 desc{};
    VnPreviewProjectHandle handle = 0;
    std::vector<std::uint8_t> body;
};

#if HARKEN_TEST_PREVIEW_ENABLED
TEST_F(StudioRuntimeApi, ChannelPeaksRetainTransientAndResetAcrossTermInit) {
    VnPreviewChannelPeaksV1 peaks{};
    peaks.structSize = sizeof(peaks);
    EXPECT_EQ(VN_NOT_INITIALIZED, VnPreview_TakeChannelPeaks(&peaks));
    ASSERT_EQ(VN_OK, VnPreview_EnableChannelPeakMetering());
    EXPECT_EQ(VN_INVALID_ARG, VnPreview_TakeChannelPeaks(nullptr));
    --peaks.structSize;
    EXPECT_EQ(VN_INVALID_ARG, VnPreview_TakeChannelPeaks(&peaks));
    peaks.structSize = sizeof(peaks);
    std::vector<float> samples(128 * 8 * 2, 0.05f);
    for (std::size_t frame = 0; frame < samples.size() / 2; ++frame) {
        samples[2 * frame + 1] = frame < 128 ? 0.2f : 0.025f;
        if (frame < 128)
            samples[2 * frame] = 0.8f;
        else if (frame >= 128 * 7)
            samples[2 * frame] = 0.0f;
    }
    const auto pcm = harkentest::InterleavedDesc(samples, 2, 48000);
    const auto soundId = harkentest::ObjectId(90);
    std::uint64_t revision = 0;
    ASSERT_EQ(VN_OK, VnPreview_RegisterPcm(soundId, &pcm, 0, 0, &revision));
    auto play = harkentest::MakePlayDesc(soundId);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, VnPreview_PlayVoiceRaw(&play, &voice));
    VnPreview_PumpNullMixer(2);
    ASSERT_EQ(VN_OK, VnPreview_TakeChannelPeaks(&peaks));
    EXPECT_EQ(2u, peaks.mixChannelCount);
    EXPECT_EQ(VN_BUS_MASTER, peaks.busIds[0]);
    EXPECT_NEAR(0.8f, peaks.channelPeaks[0], 1e-5f);
    EXPECT_NEAR(0.2f, peaks.channelPeaks[1], 1e-5f);
    const auto firstSerial = peaks.consumeSerial;
    ASSERT_EQ(VN_OK, VnPreview_TakeChannelPeaks(&peaks));
    EXPECT_GT(peaks.consumeSerial, firstSerial);
    EXPECT_NEAR(0.8f, peaks.channelPeaks[0], 1e-5f);
    EXPECT_NEAR(0.2f, peaks.channelPeaks[1], 1e-5f);
    for (const std::uint32_t blocks : {1u, 3u, 1u}) {
        VnPreview_PumpNullMixer(blocks);
        for (int poll = 0; poll < 4; ++poll) {
            ASSERT_EQ(VN_OK, VnPreview_TakeChannelPeaks(&peaks));
            EXPECT_NEAR(0.05f, peaks.channelPeaks[0], 1e-5f);
            EXPECT_NEAR(0.025f, peaks.channelPeaks[1], 1e-5f);
        }
    }
    VnPreview_PumpNullMixer(1);
    for (int poll = 0; poll < 2; ++poll) {
        ASSERT_EQ(VN_OK, VnPreview_TakeChannelPeaks(&peaks));
        EXPECT_FLOAT_EQ(0.0f, peaks.channelPeaks[0]);
        EXPECT_NEAR(0.025f, peaks.channelPeaks[1], 1e-5f);
    }
    VnPreview_PumpNullMixer(1);
    for (int poll = 0; poll < 2; ++poll) {
        ASSERT_EQ(VN_OK, VnPreview_TakeChannelPeaks(&peaks));
        EXPECT_FLOAT_EQ(0.0f, peaks.channelPeaks[0]);
        EXPECT_FLOAT_EQ(0.0f, peaks.channelPeaks[1]);
    }
    ASSERT_EQ(VN_OK, VnPreview_PlayVoiceRaw(&play, &voice));
    VnPreview_PumpNullMixer(1);
    ASSERT_EQ(VN_OK, VnPreview_TakeChannelPeaks(&peaks));
    EXPECT_NEAR(0.8f, peaks.channelPeaks[0], 1e-5f);
    EXPECT_NEAR(0.2f, peaks.channelPeaks[1], 1e-5f);
    VnPreview_PumpNullMixer(1);
    ASSERT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(VN_NOT_INITIALIZED, VnPreview_TakeChannelPeaks(&peaks));
    EXPECT_EQ(0u, peaks.consumeSerial);
    for (const float peak : peaks.channelPeaks) EXPECT_FLOAT_EQ(0.0f, peak);
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    ASSERT_EQ(VN_OK, VnPreview_EnableChannelPeakMetering());
    ASSERT_EQ(VN_OK, VnPreview_TakeChannelPeaks(&peaks));
    EXPECT_EQ(1u, peaks.consumeSerial);
    for (const float peak : peaks.channelPeaks) EXPECT_FLOAT_EQ(0.0f, peak);
}

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
    EXPECT_EQ(VN_PREVIEW_COMMIT_INVALID, VnPreview_CommitProject(handle, &generation));
    EXPECT_EQ(0u, generation);
}

TEST_F(StudioRuntimeApi, VolumeV2CommitsAuthoredMasterWithWorld) {
    auto v2 = VolumeDesc();
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV2(&v2, &handle));
    std::uint64_t generation = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(generation); ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        VnPreview_PumpNullMixer(1);
        VnPreview_DrainProjectAcks();
    }
    ASSERT_TRUE(VnPreview_ProjectIsOpen(generation));
    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(nullptr, 0, &required));
    std::vector<VnPropertyTargetInfo> rows(required == 0 ? 1 : required);
    rows[0].structSize = sizeof(VnPropertyTargetInfo);
    ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(rows.data(),
        static_cast<std::uint32_t>(rows.size()), &required));
    const auto it = std::find_if(rows.begin(), rows.begin() + required,
        [](const VnPropertyTargetInfo& row) {
            return row.scopeKind == VN_TARGET_SCOPE_BUS &&
            row.objectId == harkentest::ObjectId(2) && row.targetId == 0;
        });
    ASSERT_NE(rows.begin() + required, it);
    EXPECT_FLOAT_EQ(-3.0f, it->finalValue);
}

TEST_F(StudioRuntimeApi, SendV3RequiresExactDescriptorAndCount) {
    VnPreviewProjectDescV3 v3{};
    v3.base = VolumeDesc(4);
    v3.base.base.structSize = sizeof(v3);
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV3(&v3, &handle));
    VnPreview_CancelProject(handle);
    handle = 0;
    v3.reserved1 = 1;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV3(&v3, &handle));
    v3.reserved1 = 0;
    v3.busSendLevelCount = 1;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV3(&v3, &handle));
    v3.busSendLevelCount = 0;
    --v3.base.base.structSize;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV3(&v3, &handle));
    EXPECT_EQ(0u, handle);
}

TEST_F(StudioRuntimeApi, SendV3AcceptsCanonicalDbAndRejectsStaleFingerprint) {
    auto v2 = VolumeDesc(4);
    std::array<VnPreviewBusV1, 3> buses{{
        {harkentest::ObjectId(2), VN_BUS_MASTER},
        {harkentest::ObjectId(3), VN_BUS_MASTER},
        {VN_BUS_MASTER, VnBusId{}}}};
    std::array<VnPreviewBusVolumeV2, 3> volumes{{
        {buses[0].id, -6.0f, 3.0f, 0.0f, 0},
        {buses[1].id, 0.0f, 0.0f, 0.0f, 0},
        {VN_BUS_MASTER, 0.0f, 0.0f, 0.0f, 0}}};
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
        image.busDefinitionBusesV2.push_back({buses[i].id, buses[i].parent, i,
            volumes[i].volumeDb, volumes[i].makeUpGainDb,
            volumes[i].outputBusVolumeDb,
            harken::busfingerprint::kNeutralFaderAfterSlotV1,
            VN_CHANNEL_LAYOUT_STEREO, 0, 0, i, 1});
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
    VnPreview_CancelProject(handle);
    handle = 0;
    sendDb = -12.0f;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV3(&v3, &handle));
    sendDb = std::numeric_limits<float>::infinity();
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV3(&v3, &handle));
    EXPECT_EQ(0u, handle);
    sendDb = -6.0f;
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
    EXPECT_EQ(VN_PREVIEW_COMMIT_BACKPRESSURE,
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

    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV3(&v3, &handle));
    std::uint64_t secondGeneration = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &secondGeneration));
    handle = 0;
    for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(secondGeneration); ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        VnPreview_PumpNullMixer(1);
        VnPreview_DrainProjectAcks();
    }
    ASSERT_TRUE(VnPreview_ProjectIsOpen(secondGeneration));
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(v3.base.base.ownerId, &ownerRevision));
    EXPECT_EQ(2u, ownerRevision);
    VnPreview_PumpNullMixer(12);
    VnSnapshot afterRetry{};
    afterRetry.structSize = sizeof(afterRetry);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&afterRetry));
    ASSERT_GT(afterRetry.blockIndex, afterRefusal.blockIndex);
    EXPECT_LT(afterRetry.busPeaks[0], afterRefusal.busPeaks[0] - 0.01f);
}

TEST_F(StudioRuntimeApi, VolumeV2RejectsInvalidAbiAndValuesWithoutChangingPublishedWorld) {
    auto v2 = VolumeDesc();
    auto& volume = v2Volumes[0];
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProjectV2(&v2, &handle));
    std::uint64_t generation = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(generation); ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        VnPreview_PumpNullMixer(1);
        VnPreview_DrainProjectAcks();
    }
    ASSERT_TRUE(VnPreview_ProjectIsOpen(generation));
    std::uint64_t revision = 0;
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(desc.ownerId, &revision));
    ASSERT_EQ(1u, revision);

    v2.base.ownerBaseRevision = 1;
    v2.base.ownerRevision = 2;
    v2.base.authoringRevision = 2;
    v2.base.submissionSerial = 2;
    const auto rejects = [&](const char* caseName) {
        SCOPED_TRACE(caseName);
        handle = 99;
        const auto status = VnPreview_PrepareProjectV2(&v2, &handle);
        EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, status);
        if (status == VN_PREVIEW_PREPARE_OK) VnPreview_CancelProject(handle);
        EXPECT_EQ(0u, handle);
        EXPECT_TRUE(VnPreview_ProjectIsOpen(generation));
        std::uint64_t current = 0;
        EXPECT_TRUE(VnPreview_QueryOwnerRevision(desc.ownerId, &current));
        EXPECT_EQ(revision, current);
    };
    --v2.base.structSize;
    rejects("descriptor size");
    ++v2.base.structSize;
    VnPreviewProjectDescV1 shortDescriptor = desc;
    shortDescriptor.structSize = sizeof(shortDescriptor);
    handle = 99;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProjectV2(
        reinterpret_cast<const VnPreviewProjectDescV2*>(&shortDescriptor), &handle));
    EXPECT_EQ(0u, handle);
    v2.reserved0 = 1;
    rejects("descriptor reserved");
    v2.reserved0 = 0;
    volume.reserved0 = 1;
    rejects("bus reserved");
    volume.reserved0 = 0;
    v2.busVolumeCount = 0;
    rejects("bus count");
    v2.busVolumeCount = 2;
    volume.id = harkentest::ObjectId(7);
    rejects("bus ID");
    volume.id = harkentest::ObjectId(2);
    volume.volumeDb = std::numeric_limits<float>::quiet_NaN();
    rejects("Volume NaN");
    volume.volumeDb = 1.0e30f;
    rejects("Volume too large");
    volume.volumeDb = -6.0f;
    volume.volumeDb = -7.0f;
    rejects("Volume changed with old published fingerprint");
    volume.volumeDb = -200.0f;
    rejects("Volume below compiler range");
    volume.volumeDb = -6.0f;
    v2.base.compileTargetProfileFingerprint ^= 1;
    rejects("target profile fingerprint changed without profile");
    v2.base.compileTargetProfileFingerprint ^= 1;
    volume.makeUpGainDb = std::numeric_limits<float>::infinity();
    rejects("MakeUpGain infinity");
    volume.makeUpGainDb = 3.0f;
    volume.outputBusVolumeDb = 1.0e30f;
    rejects("OutputVolume too large");
    volume.outputBusVolumeDb = 0.0f;

    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(nullptr, 0, &required));
    std::vector<VnPropertyTargetInfo> rows(required == 0 ? 1 : required);
    rows[0].structSize = sizeof(VnPropertyTargetInfo);
    ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(rows.data(),
        static_cast<std::uint32_t>(rows.size()), &required));
    const auto it = std::find_if(rows.begin(), rows.begin() + required,
        [](const VnPropertyTargetInfo& row) {
            return row.scopeKind == VN_TARGET_SCOPE_BUS &&
            row.objectId == harkentest::ObjectId(2) && row.targetId == 0;
        });
    ASSERT_NE(rows.begin() + required, it);
    EXPECT_FLOAT_EQ(-3.0f, it->finalValue);

    desc.ownerBaseRevision = 1;
    desc.ownerRevision = 2;
    desc.authoringRevision = 2;
    desc.submissionSerial = 2;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    VnPreview_CancelProject(handle);
    handle = 0;
}

TEST_F(StudioRuntimeApi, CancelReleasesReservationsAndOldHandlesStayInvalid) {
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
        VnPreview_CancelProject(handle);
        std::uint64_t generation = 99;
        EXPECT_EQ(VN_PREVIEW_COMMIT_INVALID, VnPreview_CommitProject(handle, &generation));
        EXPECT_EQ(0u, generation);
    }
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    ASSERT_EQ(VN_OK, Vn_Term());
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    std::uint64_t generation = 99;
    EXPECT_EQ(VN_PREVIEW_COMMIT_INVALID, VnPreview_CommitProject(handle, &generation));
    EXPECT_EQ(0u, generation);
}

TEST_F(StudioRuntimeApi, InvalidDescriptorsDoNotReserveAHandle) {
    handle = 99;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProject(nullptr, &handle));
    EXPECT_EQ(0u, handle);
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProject(&desc, nullptr));
    --desc.structSize;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProject(&desc, &handle));
    ++desc.structSize;
    const auto size = desc.bodyBytes;
    desc.bodyBytes = 1;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID_WORLD, VnPreview_PrepareProject(&desc, &handle));
    desc.bodyBytes = size;
    std::array<VnPreviewBusV1, 2> duplicate{master, master};
    desc.buses = duplicate.data();
    desc.busCount = 2;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProject(&desc, &handle));
    EXPECT_EQ(0u, handle);
    duplicate[1] = {harkentest::ObjectId(9), VN_BUS_MASTER};
    const std::array<VnPreviewBusSendV1, 2> sends{{
        {duplicate[1].id, VN_BUS_MASTER}, {duplicate[1].id, VN_BUS_MASTER}}};
    desc.sends = sends.data();
    desc.sendCount = 2;
    EXPECT_EQ(VN_PREVIEW_PREPARE_INVALID, VnPreview_PrepareProject(&desc, &handle));
    EXPECT_EQ(0u, handle);
    desc.sends = nullptr;
    desc.sendCount = 0;
    desc.buses = &master;
    desc.busCount = 1;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
}

TEST_F(StudioRuntimeApi, PcmContentRevisionMatchesStudioProcessedSamples) {
    const std::array<float, 8> samples{0.25f, -0.5f, 0.75f, 1.0f, -0.25f, 0.0f, 0.5f, -1.0f};
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(pcm);
    pcm.sampleRate = 48000;
    pcm.channels = 2;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = 4;
    pcm.samplesInterleaved = samples.data();
    std::uint64_t runtimeRevision = 0, studioRevision = 0;
    ASSERT_EQ(VN_OK, VnPreview_RegisterPcm(harkentest::ObjectId(7), &pcm, 1, 3, &runtimeRevision));
    const float* channels[]{samples.data(), samples.data() + 1};
    ASSERT_TRUE(harkenstudiobuild::ComputeMediaPcmContentRevisionV1(
        pcm.sampleRate, pcm.channels, pcm.frames, channels, 2, &studioRevision, 1, 3));
    EXPECT_EQ(studioRevision, runtimeRevision);
    EXPECT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(7)));
}
#else
TEST_F(StudioRuntimeApi, DisabledGateRefusesBeforeReadingDescriptor) {
    handle = 99;
    EXPECT_EQ(VN_PREVIEW_PREPARE_UNAVAILABLE, VnPreview_PrepareProject(nullptr, &handle));
    EXPECT_EQ(0u, handle);
    EXPECT_EQ(VN_PREVIEW_PREPARE_UNAVAILABLE, VnPreview_PrepareProject(&desc, &handle));
    std::uint64_t generation = 99;
    EXPECT_EQ(VN_PREVIEW_COMMIT_UNAVAILABLE, VnPreview_CommitProject(99, &generation));
    EXPECT_EQ(0u, generation);
    EXPECT_EQ(VN_NOT_INITIALIZED, VnPreview_EnableChannelPeakMetering());
    VnPreviewChannelPeaksV1 peaks{};
    peaks.structSize = sizeof(peaks);
    peaks.channelPeaks[0] = 0.9f;
    EXPECT_EQ(VN_NOT_INITIALIZED, VnPreview_TakeChannelPeaks(&peaks));
    EXPECT_EQ(0u, peaks.consumeSerial);
    EXPECT_FLOAT_EQ(0.0f, peaks.channelPeaks[0]);
}
#endif

}
