#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/TestUtil.h"
#include "Engine.h"
#include "harken/Harken.h"

#include "ActionManager.h"
#include "AudioEngine.h"
#include "AssetPipeline/AssetRegistry.h"
#include "Components/VMixNode.h"
#include "Components/VBus.h"
#include "CurrentWorldProjectAdapter.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "PropertyEditSession.h"
#include "RuntimeSyncCoordinator.h"
#include "BusController.h"
#include "UndoCommands.h"
#include "Widgets/Blueprint/SignalPathGainEditing.h"
#include "Widgets/Blueprint/SignalPathGraph.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>
#include <cmath>
#include <memory>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

struct EngineReset {
    ~EngineReset()
    {
        VNX::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        auto& projects = VNX::ProjectManager::instance();
        if (projects.currentProject() != nullptr)
            projects.closeProject();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        VNX::AssetRegistry::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
    }
};

bool serviceUntilCurrentWorldReady(VNX::AudioEngine& engine,
                                   std::uint64_t requiredRevision = 0,
                                   std::uint64_t previousSubmissionSerial = 0)
{
    auto* runtime = harken::detail::ActiveEngine();
    auto& coordinator = engine.runtimeSyncCoordinator();
    if (requiredRevision == 0)
        requiredRevision = coordinator.currentAuthoringRevision();
    for (int attempt = 0; attempt < 128; ++attempt) {
        QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        if (runtime != nullptr) {
            if (Vn_Update() != VN_OK)
                return false;
            runtime->nullBackend.Pump(1);
            if (Vn_Update() != VN_OK)
                return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        const auto barrier = coordinator.barrier(requiredRevision);
        if (barrier.state
            == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready) {
            const auto installed = coordinator.installedWorld();
            if (installed && installed->authoringRevision >= requiredRevision
                && installed->submissionSerial > previousSubmissionSerial
                && installed->worldGeneration != 0)
                return true;
        }
        if (barrier.state
                == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed
            || barrier.state
                == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Superseded
            || barrier.state
                == VNX::RuntimeSyncCoordinator::RevisionBarrierState::RuntimeUnavailable) {
            return false;
        }
    }
    return false;
}

bool serviceNextCurrentWorld(VNX::AudioEngine& engine,
                             std::uint64_t previousRevision,
                             std::uint64_t previousSubmissionSerial)
{
    auto& coordinator = engine.runtimeSyncCoordinator();
    const auto requiredRevision = coordinator.currentAuthoringRevision();
    return requiredRevision > previousRevision
        && serviceUntilCurrentWorldReady(engine, requiredRevision,
            previousSubmissionSerial);
}

void pumpMixer()
{
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    ASSERT_EQ(Vn_Update(), VN_OK);
    runtime->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
}

harken::BusTree::Bus* busState(harkenstudio::ObjectId busId)
{
    auto* runtime = harken::detail::ActiveEngine();
    if (runtime == nullptr)
        return nullptr;
    const auto index = runtime->buses.FindIndex(harkentest::ToRuntime(busId));
    if (index == harken::kInvalidBusIndex)
        return nullptr;
    return runtime->buses.BusState(index);
}

class BusOutputVolumePreviewTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& engine = VNX::AudioEngine::instance();
        engine.shutdown();
        auto& projects = VNX::ProjectManager::instance();
        if (projects.currentProject() != nullptr)
            projects.closeProject();
        VNX::AssetRegistry::instance().clear();

        HarkenEditor::Bridge::EngineSessionConfig config;
        config.sampleRate = 48000;
        config.bufferSize = 64;
        config.maxVoices = 8;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(engine.initialize(config));
        ASSERT_TRUE(engine.startOutput());

        ASSERT_TRUE(directory_.isValid());
        VNX::ProjectConfig projectConfig;
        projectConfig.name = QStringLiteral("BusOutputVolumePreview");
        projectConfig.location = directory_.path();
        ASSERT_TRUE(projects.createProject(projectConfig));
        master_ = projects.getMasterBusItem();
        ASSERT_NE(master_, nullptr);
        child_ = projects.createItem(
            master_, QStringLiteral("Music"), VNX::ProjectItem::Type::Bus);
        ASSERT_NE(child_, nullptr);
        ASSERT_FALSE(harkenstudio::IsNull(child_->runtimeId()));
        ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
        masterId_ = projects.busRuntimeHandle(master_);
        childId_ = projects.busRuntimeHandle(child_);
        ASSERT_FALSE(harkenstudio::IsNull(masterId_));
        ASSERT_FALSE(harkenstudio::IsNull(childId_));
    }

    QTemporaryDir directory_;
    EngineReset reset_;
    VNX::ProjectItem* master_ = nullptr;
    VNX::ProjectItem* child_ = nullptr;
    harkenstudio::ObjectId masterId_{};
    harkenstudio::ObjectId childId_{};

    void startConstantVoice()
    {
        const auto soundId = harkentest::ToRuntime(harkentest::StudioObjectId(900001));
        const std::vector<float> samples(4096, 0.25f);
        const auto pcm = harkentest::MonoDesc(samples, 48000);
        ASSERT_EQ(Vn_RegisterPCM(soundId, &pcm), VN_OK);
        auto play = harkentest::MakePlayDesc(soundId);
        play.busId = harkentest::ToRuntime(childId_);
        play.loopCount = 0;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(harkentest::PlayRaw(&play, &voice), VN_OK);
        ASSERT_NE(voice, 0u);
    }

    float freshConstantSample(std::uint64_t operationBeforeBlock = 0)
    {
        auto* runtime = harken::detail::ActiveEngine();
        if (!runtime) return 0.0f;
        for (int i = 0; i < 16; ++i) {
            EXPECT_EQ(Vn_Update(), VN_OK);
            runtime->nullBackend.Pump(1);
        }
        // A synchronous Pump must advance the renderer before LastBlock is read.
        const auto before = runtime->mixer.BlocksRendered();
        runtime->nullBackend.Pump(1);
        EXPECT_GT(runtime->mixer.BlocksRendered(), before);
        EXPECT_GT(runtime->mixer.BlocksRendered(), operationBeforeBlock);
        return runtime->nullBackend.LastBlock()[0];
    }
};

} // namespace

TEST_F(BusOutputVolumePreviewTest, UnsupportedBusPublicationKeepsInstalledWorldAndAudio)
{
    auto& engine = VNX::AudioEngine::instance();
    auto& projects = VNX::ProjectManager::instance();
    startConstantVoice();
    const float reference = freshConstantSample();
    ASSERT_GT(reference, 0.0f);
    const auto installed = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(installed.has_value());
    const auto rejected = [&](bool async) {
        const auto before = harken::detail::ActiveEngine()->mixer.BlocksRendered();
        const auto request = VNX::buildCurrentWorldSyncRequest(*projects.currentProject(), std::nullopt);
        EXPECT_FALSE(request);
        if (async) {
            const auto finished = std::make_shared<bool>(false);
            VNX::ProjectManager::syncProjectToRuntimeAsync(
                projects.currentProject(), &projects, [finished]() { *finished = true; });
            ASSERT_TRUE(*finished);
        } else {
            VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            pumpMixer();
        }
        const auto after = engine.runtimeSyncCoordinator().installedWorld();
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->worldGeneration, installed->worldGeneration);
        EXPECT_EQ(after->submissionSerial, installed->submissionSerial);
        EXPECT_EQ(after->authoringRevision, installed->authoringRevision);
        EXPECT_NEAR(freshConstantSample(before), reference, 0.002f);
    };
    child_->comp<VNX::VBus>()->pan = 0.25f;
    rejected(false);
    rejected(true);
    child_->comp<VNX::VBus>()->pan = 0.0f;
    child_->comp<VNX::VMixNode>()->pitch = 300.0f;
    child_->playablePropertyPresence.Set(harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    rejected(false);
    rejected(true);
    child_->comp<VNX::VMixNode>()->pitch = 0.0f;
    child_->comp<VNX::VBus>()->listenerRelativeRouting = true;
    rejected(false);
    rejected(true);
    child_->comp<VNX::VBus>()->listenerRelativeRouting = false;
    EXPECT_TRUE(VNX::buildCurrentWorldSyncRequest(*projects.currentProject(), std::nullopt));
}

TEST_F(BusOutputVolumePreviewTest, AuthoredOutputAndApiOverlayStaySeparate)
{
    auto& engine = VNX::AudioEngine::instance();
    auto& pm = VNX::ProjectManager::instance();
    auto* childMix = child_->comp<VNX::VMixNode>();
    auto* masterMix = master_->comp<VNX::VMixNode>();
    ASSERT_NE(childMix, nullptr);
    ASSERT_NE(masterMix, nullptr);
    auto* childBus = busState(childId_);
    auto* masterBus = busState(masterId_);
    ASSERT_NE(childBus, nullptr);
    ASSERT_NE(masterBus, nullptr);
    const float childGainBefore = childBus->gain.target;
    const float masterGainBefore = masterBus->gain.target;
    const auto prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior.has_value());

    ASSERT_TRUE(pm.applyBusPropertyLive(
        child_, QStringLiteral("outputBusVolume"), -6.0f));
    ASSERT_TRUE(pm.applyBusPropertyLive(
        master_, QStringLiteral("outputBusVolume"), -3.0f));
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision,
        prior->submissionSerial));
    pumpMixer();

    EXPECT_FLOAT_EQ(childMix->outputBusVolume, -6.0f);
    EXPECT_FLOAT_EQ(masterMix->outputBusVolume, -3.0f);
    EXPECT_FLOAT_EQ(pm.effectiveOutputVolumeDbForItem(child_), -6.0f);
    EXPECT_FLOAT_EQ(pm.effectiveOutputVolumeDbForItem(master_), -3.0f);
    EXPECT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-6.0f), 1e-5f);
    EXPECT_NEAR(masterBus->outputGain.target, VNX::VMixNode::dbToLinear(-3.0f), 1e-5f);
    EXPECT_FLOAT_EQ(childBus->gain.target, childGainBefore);
    EXPECT_FLOAT_EQ(masterBus->gain.target, masterGainBefore);
    EXPECT_EQ(engine.busTopologyState(),
              HarkenEditor::Bridge::BusTopologyState::Ready);

    engine.setBusOutputVolumeDbById(childId_, -12.0f);
    pumpMixer();
    EXPECT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-18.0f), 1e-5f);
    EXPECT_FLOAT_EQ(childBus->gain.target, childGainBefore);
}

TEST_F(BusOutputVolumePreviewTest, AuthoredUpdatePreservesApiOutputOverlay)
{
    auto& engine = VNX::AudioEngine::instance();
    auto& pm = VNX::ProjectManager::instance();
    const auto prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior.has_value());
    ASSERT_TRUE(pm.applyBusPropertyLive(
        child_, QStringLiteral("outputBusVolume"), -6.0f));
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision,
        prior->submissionSerial));
    pumpMixer();
    auto* childBus = busState(childId_);
    ASSERT_NE(childBus, nullptr);
    ASSERT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-6.0f), 1e-5f);
    const float gainBefore = childBus->gain.target;
    engine.setBusOutputVolumeDbById(childId_, -3.0f);
    pumpMixer();
    ASSERT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-9.0f), 1e-5f);
    const auto beforeUpdate = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(beforeUpdate.has_value());
    ASSERT_TRUE(pm.applyBusPropertyLive(
        child_, QStringLiteral("outputBusVolume"), -9.0f));
    ASSERT_TRUE(serviceNextCurrentWorld(engine, beforeUpdate->authoringRevision,
        beforeUpdate->submissionSerial));
    pumpMixer();
    childBus = busState(childId_);
    ASSERT_NE(childBus, nullptr);
    EXPECT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-12.0f), 1e-5f);
    EXPECT_FLOAT_EQ(childBus->gain.target, gainBefore);
}

TEST_F(BusOutputVolumePreviewTest, BusControllerRefreshDoesNotDuplicateAuthoredGainInNullAudio)
{
    auto& engine = VNX::AudioEngine::instance();
    auto& pm = VNX::ProjectManager::instance();
    startConstantVoice();
    const float reference = freshConstantSample();
    ASSERT_GT(std::abs(reference), 0.05f);
    VNX::BusController controller(nullptr);
    auto operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    engine.setBusOutputVolumeDbById(childId_, -3.0f);

    auto prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior);
    ASSERT_TRUE(pm.applyBusPropertyLive(child_, QStringLiteral("outputBusVolume"), -6.0f));
    controller.handleBusParameterChanged(child_->objectID, QStringLiteral("volume"));
    controller.handleBusParameterChanged(child_->objectID, QStringLiteral("outputBusVolume"));
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision, prior->submissionSerial));
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-9.0f), 0.002f);

    operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior);
    ASSERT_TRUE(pm.applyBusPropertyLive(child_, QStringLiteral("outputBusVolume"), -9.0f));
    controller.handleBusParameterChanged(child_->objectID, QStringLiteral("volume"));
    controller.handleBusParameterChanged(child_->objectID, QStringLiteral("outputBusVolume"));
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision, prior->submissionSerial));
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-12.0f), 0.002f);

    operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    engine.setBusOutputVolumeDbById(childId_, 0.0f);
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-9.0f), 0.002f);

    operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior);
    ASSERT_TRUE(pm.applyBusPropertyLive(child_, QStringLiteral("volume"), -2.0f));
    controller.handleBusParameterChanged(child_->objectID, QStringLiteral("volume"));
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision, prior->submissionSerial));
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-11.0f), 0.002f);
}

TEST_F(BusOutputVolumePreviewTest, SignalPathOutputPreviewCancelCommitUndoChangesFreshNullAudio)
{
    auto& engine = VNX::AudioEngine::instance();
    auto& pm = VNX::ProjectManager::instance();
    startConstantVoice();
    const float reference = freshConstantSample();
    ASSERT_GT(std::abs(reference), 0.05f);
    auto operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    engine.setBusOutputVolumeDbById(childId_, -3.0f);
    const auto graph = VNX::buildSignalPathGraph(*pm.currentProject(), {}, {}, {child_->objectID});
    ASSERT_TRUE(graph.ok());
    size_t outputEdge = graph.edges.size();
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        if (edge.kind == VNX::SignalPathEdgeKind::Output && edge.routingSource
            && edge.routingSource->objectGuid == child_->guid) {
            outputEdge = index;
            break;
        }
    }
    ASSERT_LT(outputEdge, graph.edges.size());

    auto prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior);
    VNX::SignalPathGainEditSession cancelled(graph, outputEdge);
    ASSERT_TRUE(cancelled.isActive());
    cancelled.preview(-6.0);
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision, prior->submissionSerial));
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-9.0f), 0.002f);

    operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior);
    cancelled.cancel();
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision, prior->submissionSerial));
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-3.0f), 0.002f);

    operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior);
    VNX::SignalPathGainEditSession committed(graph, outputEdge);
    ASSERT_TRUE(committed.isActive());
    committed.preview(-6.0);
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision, prior->submissionSerial));
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-9.0f), 0.002f);
    operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior);
    committed.commit();
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision, prior->submissionSerial));
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-9.0f), 0.002f);

    operationBeforeBlock = harken::detail::ActiveEngine()->mixer.BlocksRendered();
    prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision, prior->submissionSerial));
    EXPECT_NEAR(freshConstantSample(operationBeforeBlock), reference * VNX::VMixNode::dbToLinear(-3.0f), 0.002f);
}

TEST_F(BusOutputVolumePreviewTest, TopologyChangeThenCurrentWorldReplayKeepsMixNode)
{
    auto& engine = VNX::AudioEngine::instance();
    auto& pm = VNX::ProjectManager::instance();
    const auto prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior.has_value());
    ASSERT_TRUE(pm.applyBusPropertyLive(
        child_, QStringLiteral("outputBusVolume"), -9.0f));
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision,
        prior->submissionSerial));
    pumpMixer();
    auto* childBus = busState(childId_);
    ASSERT_NE(childBus, nullptr);
    ASSERT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-9.0f), 1e-5f);

    auto* extra = pm.createItem(
        master_, QStringLiteral("Extra"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(extra, nullptr);
    const auto topologyRevision = engine.runtimeSyncCoordinator()
        .currentAuthoringRevision();
    ASSERT_GT(topologyRevision, prior->authoringRevision);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine, topologyRevision,
        prior->submissionSerial));
    pumpMixer();

    childBus = busState(childId_);
    ASSERT_NE(childBus, nullptr);
    EXPECT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-9.0f), 1e-5f);
    EXPECT_FLOAT_EQ(child_->comp<VNX::VMixNode>()->outputBusVolume, -9.0f);
}

TEST_F(BusOutputVolumePreviewTest, PreviewDoesNotLetOverlayCoverMixNodeLive)
{
    auto& pm = VNX::ProjectManager::instance();
    auto& engine = VNX::AudioEngine::instance();
    const auto prior = engine.runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior.has_value());
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*child_);
    ASSERT_TRUE(locator.has_value());
    const auto propertyId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    const auto platform = QUuid::createUuid();
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *child_, *locator, propertyId, -12.0, platform));
    EXPECT_FLOAT_EQ(child_->comp<VNX::VMixNode>()->outputBusVolume, 0.0f);

    QVector<VNX::ProjectPlatformId> platforms{platform};
    VNX::PropertyEditSession session(
        child_, QStringLiteral("outputBusVolume"), QVariant(0.0f),
        nullptr, platforms);
    session.preview(QVariant(-6.0f));
    ASSERT_TRUE(serviceNextCurrentWorld(engine, prior->authoringRevision,
        prior->submissionSerial));
    pumpMixer();

    EXPECT_FLOAT_EQ(child_->comp<VNX::VMixNode>()->outputBusVolume, -6.0f);
    EXPECT_FLOAT_EQ(pm.effectiveOutputVolumeDbForItem(child_), -6.0f);
    auto* childBus = busState(childId_);
    ASSERT_NE(childBus, nullptr);
    EXPECT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-6.0f), 1e-5f);
    const auto overlay = VNX::PlayablePropertyAuthoringV1::readPlatformOverlay(
        *child_, *locator, propertyId, platform);
    ASSERT_TRUE(overlay.has_value());
    EXPECT_TRUE(overlay->present);
    EXPECT_DOUBLE_EQ(overlay->value.toDouble(), -12.0);
}

TEST_F(BusOutputVolumePreviewTest, SetItemPropertyWithPlatformIdsWritesMixNodeNotOverlay)
{
    auto& pm = VNX::ProjectManager::instance();
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerProjectActions(actions, &pm, nullptr, {});

    const QStringList platformIds = pm.getTargetPlatforms();
    ASSERT_FALSE(platformIds.isEmpty());
    const QString platformId = platformIds.first();
    const auto platform = QUuid::fromString(platformId);
    ASSERT_FALSE(platform.isNull());
    const auto prior = VNX::AudioEngine::instance()
        .runtimeSyncCoordinator().installedWorld();
    ASSERT_TRUE(prior.has_value());

    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*child_);
    ASSERT_TRUE(locator.has_value());
    const auto propertyId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *child_, *locator, propertyId, -12.0, platform));
    EXPECT_FLOAT_EQ(child_->comp<VNX::VMixNode>()->outputBusVolume, 0.0f);

    const VNX::ActionResult result = actions.execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(child_->objectID)},
            {QStringLiteral("property"), QStringLiteral("outputBusVolume")},
            {QStringLiteral("value"), -6.0},
            {QStringLiteral("platform_ids"), QJsonArray{platformId}},
        },
        VNX::ActionSource::Internal);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_TRUE(serviceNextCurrentWorld(VNX::AudioEngine::instance(),
        prior->authoringRevision, prior->submissionSerial));
    pumpMixer();

    EXPECT_FLOAT_EQ(child_->comp<VNX::VMixNode>()->outputBusVolume, -6.0f);
    EXPECT_FLOAT_EQ(pm.effectiveOutputVolumeDbForItem(child_), -6.0f);
    auto* childBus = busState(childId_);
    ASSERT_NE(childBus, nullptr);
    EXPECT_NEAR(childBus->outputGain.target, VNX::VMixNode::dbToLinear(-12.0f), 1e-5f);
    const auto overlay = VNX::PlayablePropertyAuthoringV1::readPlatformOverlay(
        *child_, *locator, propertyId, platform);
    ASSERT_TRUE(overlay.has_value());
    EXPECT_TRUE(overlay->present);
    EXPECT_DOUBLE_EQ(overlay->value.toDouble(), -12.0);
}
