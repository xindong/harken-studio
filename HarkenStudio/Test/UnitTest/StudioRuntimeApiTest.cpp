#include <harken/Harken.h>
#include "UnitTest/ObjectIdTestSupport.h"
#include "UnitTest/TestUtil.h"
#include <harken_preview/PreviewProject.h>
#include <harken_current_world_compiler/CurrentWorldCompiler.h>
#include <harken_studio_build/WorldCodec.h>
#include <harken/core/ChannelLayout.h>
#include <harken/media/PackLogicImage.h>

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
#endif
}
