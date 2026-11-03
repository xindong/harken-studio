#include "Common/ObjectIdTestSupport.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/CurrentWorldArtifact.h"
#include "harken/core/detail/CurrentWorldValidation.h"

#include <gtest/gtest.h>
#include <array>
#include <limits>

namespace {
using namespace harken;

struct SignalWorld {
    std::array<SignalRelationV1, 2> relations{};
    std::array<SignalReceiverV1, 2> receivers{};
    ContainerWorldInstallViewV1 world{};
    SignalWorld() {
        world.sourceBodyVintage = kCurrentWorldBodyVintageV20;
        for (std::uint32_t i = 0; i < 2; ++i) {
            receivers[i].target.stableId = harkentest::ObjectId(i + 1);
            relations[i].stableId = harkentest::ObjectId(i + 10);
            relations[i].source.stableId = harkentest::ObjectId(2 - i);
            relations[i].target = receivers[i].target;
        }
        world.signalRelations = relations.data();
        world.signalRelationCount = 2;
        world.signalReceivers = receivers.data();
        world.signalReceiverCount = 2;
        Refresh();
    }
    void Refresh() {
        world.identity.capabilityMask = detail::ComputeCurrentWorldLocalCapabilityMaskV1(world);
        world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV7(world);
    }
};

TEST(CurrentWorldSignal, V20OwnsRoundtripTablesAndFingerprintIncludesEveryConfiguration) {
    SignalWorld f;
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::Ok);
    std::vector<std::uint8_t> bytes, again;
    ASSERT_TRUE(EncodeCurrentWorldBodyV20(f.world, bytes));
    CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), 20, CurrentWorldDecodeMode::StrictArtifact));
    const auto* view = image.ProductionView();
    ASSERT_NE(view, nullptr);
    ASSERT_EQ(view->signalRelationCount, 2u);
    ASSERT_EQ(view->signalReceiverCount, 2u);
    ASSERT_TRUE(EncodeCurrentWorldBodyV20(*view, again));
    EXPECT_EQ(again, bytes);
    EXPECT_NE(view->signalRelations, f.relations.data());
    EXPECT_NE(view->signalReceivers, f.receivers.data());
    const auto original = ComputeCurrentWorldFingerprintV7(*view);
    f.relations[0].sendGainDb = -6;
    EXPECT_NE(original, ComputeCurrentWorldFingerprintV7(f.world));
    f.relations[0].sendGainDb = 0;
    f.receivers[0].attackMs = 10;
    EXPECT_NE(original, ComputeCurrentWorldFingerprintV7(f.world));
    EXPECT_EQ(original, ComputeCurrentWorldFingerprintV7(*view));
    ASSERT_TRUE(EncodeCurrentWorldArtifactV1(*view, bytes));
    CurrentWorldArtifactHeaderV1 header;
    ASSERT_EQ(InspectCurrentWorldArtifactHeaderV1(bytes.data(), bytes.size(), bytes.size(), header),
              CurrentWorldLoadStatusV1::Ok);
    EXPECT_EQ(header.bodyVintage, 20u);
    CurrentWorldBodyImage artifact;
    ASSERT_EQ(DecodeCurrentWorldArtifactBodyV1(header,
        bytes.data() + kCurrentWorldArtifactHeaderBytes,
        bytes.size() - kCurrentWorldArtifactHeaderBytes, artifact), CurrentWorldLoadStatusV1::Ok);
    EXPECT_EQ(artifact.ProductionView()->signalRelationCount, 2u);
}

TEST(CurrentWorldSignal, TruncatedOrOversizedTablesPreservePreviousDecodedImage) {
    SignalWorld f;
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeCurrentWorldBodyV20(f.world, bytes));
    CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), 20, CurrentWorldDecodeMode::StrictArtifact));
    const auto* prior = image.ProductionView();
    const auto suffix = bytes.size() - 8 - 2 * 104 - 2 * 56;
    for (std::size_t n = suffix; n < bytes.size(); ++n) {
        EXPECT_FALSE(image.Decode(bytes.data(), n, 20, CurrentWorldDecodeMode::StrictArtifact));
        EXPECT_EQ(image.ProductionView(), prior);
    }
    for (std::size_t n = 0; n < 8; ++n) bytes[suffix + n] = 0xff;
    EXPECT_FALSE(image.Decode(bytes.data(), bytes.size(), 20, CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(image.ProductionView(), prior);
}

TEST(CurrentWorldSignal, OldBodiesCannotSilentlyDiscardSignalTablesOrEnableMusic) {
    SignalWorld f;
    std::vector<std::uint8_t> bytes;
    for (const auto vintage : {18u, 19u}) {
        f.world.sourceBodyVintage = vintage;
        EXPECT_FALSE(vintage == 18 ? EncodeCurrentWorldShippingBody(f.world, bytes)
                                  : EncodeCurrentWorldBodyV19(f.world, bytes));
    }
    ContainerWorldInstallViewV1 empty;
    empty.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV5(empty);
    ASSERT_TRUE(EncodeCurrentWorldShippingBody(empty, bytes));
    CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), 18, CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(image.ProductionView()->signalRelationCount, 0u);
    EXPECT_EQ(image.ProductionView()->signalReceiverCount, 0u);
    EXPECT_EQ(ComputeCurrentWorldFingerprintV5(*image.ProductionView()),
              empty.identity.runtimeSemanticFingerprint);
    empty.sourceBodyVintage = 19;
    empty.identity.capabilityMask = kWorldCapabilityEntryScheduling;
    empty.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV6(empty);
    EXPECT_EQ(ValidateCurrentWorldV1(empty), WorldInstallStatus::CapabilityUnavailable);
    ASSERT_TRUE(EncodeCurrentWorldBodyV19(empty, bytes));
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), 19, CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(image.ProductionView()->signalReceiverCount, 0u);
    empty.sourceBodyVintage = 20;
    empty.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV7(empty);
    EXPECT_EQ(ValidateCurrentWorldV1(empty), WorldInstallStatus::InvalidFormat);
}

TEST(CurrentWorldSignal, RejectsDuplicateTargetsUnknownConsumersAndInvalidParameters) {
    SignalWorld f;
    const auto rejected = [&] {
        f.Refresh();
        EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::InvalidWorld);
    };
    f.receivers[1].target = f.receivers[0].target;
    rejected();
    f.receivers[1].target = f.relations[1].target;
    f.relations[1].stableId = f.relations[0].stableId;
    rejected();
    f.relations[1].stableId = harkentest::ObjectId(11);
    f.relations[0].consumer = static_cast<SignalConsumerKindV1>(2);
    rejected();
    f.relations[0].consumer = SignalConsumerKindV1::Ducking;
    f.receivers[0].algorithm = 2;
    rejected();
    f.receivers[0].algorithm = 1;
    for (const auto value : {-97.0f, 25.0f, std::numeric_limits<float>::quiet_NaN()}) {
        f.relations[0].sendGainDb = value;
        rejected();
    }
    f.relations[0].sendGainDb = 0;
    f.receivers[0].releaseMs = 5001;
    rejected();
    f.receivers[0].releaseMs = 1;
    f.receivers[0].attackMs = .09f;
    rejected();
    f.receivers[0].attackMs = .1f;
    f.relations[0].sendGainDb = -96;
    f.relations[1].sendGainDb = 24;
    f.Refresh();
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::Ok);
}

TEST(CurrentWorldSignal, MissingContentAndReceiverRequireFinalClosureValidation) {
    SignalWorld f;
    f.relations[0].source.kind = SignalEndpointKindV1::Sound;
    f.relations[0].source.expectedPropertyFingerprint = 123;
    f.Refresh();
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::Ok);
    EXPECT_EQ(ValidateCurrentWorldV1(f.world), WorldInstallStatus::InvalidWorld);
    f.relations[0].source.kind = SignalEndpointKindV1::Bus;
    f.relations[0].source.expectedPropertyFingerprint = 0;
    f.world.signalReceiverCount = 1;
    f.Refresh();
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::Ok);
    EXPECT_EQ(ValidateCurrentWorldV1(f.world), WorldInstallStatus::InvalidWorld);
}
} // namespace
