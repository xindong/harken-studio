#include "Common/ObjectIdTestSupport.h"

#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentWorldCodec.h"
#include <harken/HarkenDevelopment.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace {

class PreviewProjectTransaction : public testing::Test {
protected:
    void SetUp() override {
        config.structSize = sizeof(config);
        config.deviceMode = VN_DEVICE_NULL;
        config.sampleRate = 48000;
        config.blockFrames = 128;
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        harken::ContainerWorldInstallViewV1 world{};
        world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(world);
        ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(world, body));
        desc.structSize = sizeof(desc);
        desc.bodyVintage = harken::kCurrentWorldShippingBodyVintage;
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
    }

    void TearDown() override {
        VnPreview_CancelProject(handle);
        EXPECT_EQ(VN_OK, Vn_Term());
    }

    bool Opens(std::uint64_t generation) {
        for (int i = 0; i < 16 && !VnPreview_ProjectIsOpen(generation); ++i) {
            if (Vn_Update() != VN_OK) return false;
            VnPreview_PumpNullMixer(1);
            VnPreview_DrainProjectAcks();
        }
        return VnPreview_ProjectIsOpen(generation) != 0;
    }

    VnConfig config{};
    VnPreviewBusV1 master{VN_BUS_MASTER, VN_BUS_MASTER};
    VnPreviewProjectDescV1 desc{};
    VnPreviewProjectHandle handle = 0;
    std::vector<std::uint8_t> body;
};

TEST_F(PreviewProjectTransaction, PreparedInputIsOwnedAndCommitsOnlyOnce) {
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    ASSERT_NE(0u, handle);
    std::fill(body.begin(), body.end(), 0);
    master = {};
    std::uint64_t generation = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    ASSERT_NE(0u, generation);
    EXPECT_TRUE(Opens(generation));
    std::uint64_t revision = 0;
    EXPECT_TRUE(VnPreview_QueryOwnerRevision(desc.ownerId, &revision));
    EXPECT_EQ(desc.ownerRevision, revision);
    EXPECT_NE(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &generation));
    EXPECT_EQ(0u, generation);
}

TEST_F(PreviewProjectTransaction, CancelAndRestartInvalidateEarlierHandles) {
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

TEST_F(PreviewProjectTransaction, RefusedCommitLeavesThePublishedProjectIntact) {
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    std::uint64_t first = 0;
    ASSERT_EQ(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &first));
    handle = 0;
    ASSERT_TRUE(Opens(first));

    desc.ownerBaseRevision = 1;
    desc.ownerRevision = 2;
    desc.authoringRevision = 2;
    desc.submissionSerial = 2;
    ASSERT_EQ(VN_PREVIEW_PREPARE_OK, VnPreview_PrepareProject(&desc, &handle));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(91), VN_BUS_MASTER));
    VnPreview_PumpNullMixer(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    std::uint64_t refused = 99;
    EXPECT_NE(VN_PREVIEW_COMMIT_OK, VnPreview_CommitProject(handle, &refused));
    EXPECT_EQ(0u, refused);
    EXPECT_TRUE(VnPreview_ProjectIsOpen(first));
    std::uint64_t revision = 0;
    ASSERT_TRUE(VnPreview_QueryOwnerRevision(desc.ownerId, &revision));
    EXPECT_EQ(1u, revision);
}

TEST_F(PreviewProjectTransaction, InvalidDescriptorsDoNotReserveAHandle) {
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

}
