#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include "CompileTargetProfileCatalog.h"
#include "ProjectTypes.h"

#include <harken/core/CurrentWorld.h>
#include <harken_bus_compiler/BusCompiler.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace {

std::shared_ptr<HarkenEditor::Bridge::CurrentWorldSyncRequest>
makeValidRequest()
{
    auto request =
        std::make_shared<HarkenEditor::Bridge::CurrentWorldSyncRequest>();
    const harkenprojectschema::AuthoringGuid masterGuid{0x4d4153544552, 1};
    const auto identity = harkenprojectschema::BuildRuntimeIdentityIndexV1({
        {1, {masterGuid,
             harkenprojectschema::RuntimeIdentityKindV1::Bus,
             masterGuid,
             {masterGuid, harkenprojectschema::RuntimeIdentityKindV1::Bus,
              0, masterGuid}}},
    });
    request->identityIndex = identity.index;
    VNX::ProjectData targetProject;
    targetProject.sampleRate = 48000;
    targetProject.channelConfig = VNX::AudioChannelConfig::Stereo_2_0;
    request->compileTargetProfile =
        VNX::CompileTargetProfileCatalog::resolve(targetProject);
    request->project.identityIndex = request->identityIndex;
    request->project.compileTargetProfile = request->compileTargetProfile;
    harkenbuscompiler::BusCompilerInputV1 busInput;
    busInput.identityIndex = request->identityIndex;
    busInput.compileTargetProfile = request->compileTargetProfile;
    busInput.buses.push_back({
        masterGuid, harkenstudio::kMasterBusObjectId, 0.0f, 8, VN_CHANNEL_LAYOUT_DEFAULT,
        {masterGuid, "Busses/Master.vp", "/volume"}});
    request->busResult =
        harkenbuscompiler::CompileBusProjectV1(busInput).result;
    return request;
}

class CurrentWorldSyncBridgeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
        HarkenEditor::Bridge::EngineSessionConfig config;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        session = HarkenEditor::Bridge::EngineSession::create(config);
        ASSERT_NE(session, nullptr);
        HarkenEditor::Bridge::setProjectGenerationTestBackendForTesting(
            &generationBackend);
    }

    void TearDown() override
    {
        session.reset();
        HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
    }

    std::unique_ptr<HarkenEditor::Bridge::EngineSession> session;
    HarkenEditor::Bridge::ProjectGenerationTestBackend generationBackend;
};

}

TEST_F(CurrentWorldSyncBridgeTest,
       PendingCommitYieldsOneTaggedTerminalCompletionAfterService)
{
    generationBackend.commitStatus =
        HarkenEditor::Bridge::ProjectGenerationTestCommitStatus::Backpressure;
    generationBackend.openImmediatelyOnCommitOk = true;
    generationBackend.installedWorldGeneration = 47;

    const HarkenEditor::Bridge::CurrentWorldSyncToken token{11, 12, 13, 14};
    auto request = makeValidRequest();
    const auto submitted =
        HarkenEditor::Bridge::syncCurrentWorld(std::move(request), token);

    ASSERT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending,
              submitted.status);
    EXPECT_EQ(token, submitted.token);
    EXPECT_EQ(1u, generationBackend.commitCalls);

    session->update();
    EXPECT_FALSE(HarkenEditor::Bridge::takeCurrentWorldSyncCompletion().has_value());

    generationBackend.commitStatus =
        HarkenEditor::Bridge::ProjectGenerationTestCommitStatus::Ok;
    session->update();
    const auto completed =
        HarkenEditor::Bridge::takeCurrentWorldSyncCompletion();
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok,
              completed->status);
    EXPECT_EQ(token, completed->token);
    EXPECT_EQ(47u, completed->worldGeneration);
    EXPECT_FALSE(HarkenEditor::Bridge::takeCurrentWorldSyncCompletion().has_value());
}

TEST_F(CurrentWorldSyncBridgeTest,
       DifferentTokenCancelsPreviousHoldInsteadOfIgnoringTheNewRequest)
{
    generationBackend.commitStatus =
        HarkenEditor::Bridge::ProjectGenerationTestCommitStatus::Backpressure;
    generationBackend.installedWorldGeneration = 71;

    const HarkenEditor::Bridge::CurrentWorldSyncToken firstToken{21, 22, 23, 24};
    const HarkenEditor::Bridge::CurrentWorldSyncToken secondToken{21, 25, 23, 26};

    auto firstRequest = makeValidRequest();
    const auto first =
        HarkenEditor::Bridge::syncCurrentWorld(std::move(firstRequest), firstToken);
    ASSERT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending,
              first.status);

    generationBackend.commitStatus =
        HarkenEditor::Bridge::ProjectGenerationTestCommitStatus::Ok;
    auto secondRequest = makeValidRequest();
    const auto second = HarkenEditor::Bridge::syncCurrentWorld(
        std::move(secondRequest), secondToken);
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending,
              second.status);
    EXPECT_EQ(secondToken, second.token);

    const auto superseded = HarkenEditor::Bridge::takeCurrentWorldSyncCompletion();
    ASSERT_TRUE(superseded.has_value());
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Superseded,
              superseded->status);
    EXPECT_EQ(firstToken, superseded->token);

    session->update();
    const auto completed = HarkenEditor::Bridge::takeCurrentWorldSyncCompletion();
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok, completed->status);
    EXPECT_EQ(secondToken, completed->token);
    EXPECT_EQ(71u, completed->worldGeneration);
}

TEST_F(CurrentWorldSyncBridgeTest,
       DiscardResetsQueuedOpenCompletion)
{
    generationBackend.installedWorldGeneration = 91;
    generationBackend.openImmediatelyOnCommitOk = true;

    const HarkenEditor::Bridge::CurrentWorldSyncToken token{31, 32, 33, 34};
    auto request = makeValidRequest();
    const auto submitted =
        HarkenEditor::Bridge::syncCurrentWorld(std::move(request), token);
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending,
              submitted.status);
    session->update();

    const auto discarded = HarkenEditor::Bridge::discardPendingCurrentWorld();
    ASSERT_TRUE(discarded.has_value());
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Cancelled,
              discarded->status);
    EXPECT_FALSE(HarkenEditor::Bridge::takeCurrentWorldSyncCompletion().has_value());
    EXPECT_TRUE(generationBackend.cancelCalled
                || !HarkenEditor::Bridge::discardPendingCurrentWorld().has_value());
}

TEST_F(CurrentWorldSyncBridgeTest,
       ClearCurrentWorldKeepsMasterSoFillDoesNotCompileFail)
{
    HarkenEditor::Bridge::setProjectGenerationTestBackendForTesting(nullptr);
    const HarkenEditor::Bridge::CurrentWorldSyncToken token{41, 42, 43, 44};
    auto cleared = HarkenEditor::Bridge::clearCurrentWorld(token);
    EXPECT_NE(HarkenEditor::Bridge::CurrentWorldSyncStatus::CompileFailed,
              cleared.status);
    for (int attempt = 0;
         attempt < 8
         && cleared.status
             == HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending;
         ++attempt) {
        session->update();
        if (const auto completed =
                HarkenEditor::Bridge::takeCurrentWorldSyncCompletion()) {
            cleared = *completed;
            break;
        }
    }
    EXPECT_NE(HarkenEditor::Bridge::CurrentWorldSyncStatus::CompileFailed,
              cleared.status);
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok, cleared.status)
        << static_cast<std::uint32_t>(cleared.status);
}
