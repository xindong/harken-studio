#include "UnitTest/StudioObjectIdTestSupport.h"
#include "AudioEngine.h"
#include "CompileTargetProfileCatalog.h"
#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/MediaPool.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Components/VAction.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "PropertyEditSession.h"
#include "UndoCommands.h"
#include "RuntimeSyncCoordinator.h"
#include "AuditionManager.h"
#include "Core/Build/CurrentWorldCompile.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/ProjectSyncBridge.h"
#include "Core/Build/PackagingBridge.h"
#include "RuntimeBridge/PreviewBridge.h"
#include "RuntimeBridge/RuntimeLink.h"
#include "harken/Harken.h"
#include <harken_bus_compiler/BusCompiler.h>
#include <harken/HarkenDevelopment.h>

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

bool pendingDiscardObserved = false;
bool runtimeActiveDuringPendingDiscard = false;
HarkenEditor::Bridge::CurrentWorldSyncToken discardedPendingToken;

HarkenEditor::Bridge::CurrentWorldTestInstallResult backpressuredWorldInstall() noexcept
{
    HarkenEditor::Bridge::CurrentWorldTestInstallResult result;
    result.status = HarkenEditor::Bridge::CurrentWorldTestInstallResult::Status::Backpressure;
    return result;
}

void observePendingDiscard(
    HarkenEditor::Bridge::CurrentWorldSyncToken token,
    bool runtimeActive) noexcept
{
    pendingDiscardObserved = true;
    runtimeActiveDuringPendingDiscard = runtimeActive;
    discardedPendingToken = token;
}

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

std::shared_ptr<HarkenEditor::Bridge::CurrentWorldSyncRequest>
makeValidCurrentWorldRequest()
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

bool serviceUntilCurrentWorldReady(VNX::AudioEngine& engine)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < 5000) {
        QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        if (Vn_Update() != VN_OK)
            return false;
        VnPreview_PumpNullMixer(1);
        if (Vn_Update() != VN_OK)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        const auto& coordinator = engine.runtimeSyncCoordinator();
        const auto barrier = coordinator.barrier(
            coordinator.currentAuthoringRevision());
        if (barrier.state
            == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready) {
            return true;
        }
        if (barrier.state
                == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed
            || barrier.state
                == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Superseded
            || barrier.state
                == VNX::RuntimeSyncCoordinator::RevisionBarrierState::RuntimeUnavailable) {
            ADD_FAILURE() << "World state=" << static_cast<int>(barrier.state)
                          << " failure=" << static_cast<int>(barrier.failure);
            return false;
        }
        QThread::msleep(1);
    }
    const auto& coordinator = engine.runtimeSyncCoordinator();
    const auto barrier = coordinator.barrier(coordinator.currentAuthoringRevision());
    ADD_FAILURE() << "World remained pending: revision=" << barrier.requiredRevision
                  << " installed=" << barrier.installedRevision
                  << " outstanding=" << coordinator.hasOutstandingSubmission();
    return false;
}

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

void writeWavF32(const QString& path, const std::vector<float>& samples,
                  uint16_t channels = 1)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint32_t fmtSize = 18;
    const uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);

    QByteArray bytes;
    bytes.reserve(static_cast<int>(riffSize + 8));
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, 48000);
    appendU32(bytes, 48000 * channels * sizeof(float));
    appendU16(bytes, channels * sizeof(float));
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, static_cast<uint32_t>(samples.size() / channels));
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()),
                 static_cast<qsizetype>(dataBytes));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

void writeWavPcm24(const QString& path, uint32_t frameCount)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    constexpr uint16_t channels = 2;
    constexpr uint16_t bytesPerSample = 3;
    constexpr uint16_t blockAlign = channels * bytesPerSample;
    constexpr uint32_t sampleRate = 48000;
    const uint32_t dataBytes = frameCount * blockAlign;
    const uint32_t riffSize = 4 + (8 + 16) + (8 + dataBytes);

    QByteArray bytes;
    bytes.reserve(static_cast<int>(riffSize + 8));
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, 16);
    appendU16(bytes, 1);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 24);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const int32_t sample = (frame & 1u) == 0 ? 0x100000 : -0x100000;
        for (uint16_t channel = 0; channel < channels; ++channel) {
            bytes.append(static_cast<char>(sample & 0xFF));
            bytes.append(static_cast<char>((sample >> 8) & 0xFF));
            bytes.append(static_cast<char>((sample >> 16) & 0xFF));
        }
    }
    ASSERT_EQ(file.write(bytes), bytes.size());
}

struct AudioEnginePreviewReset {
    ~AudioEnginePreviewReset()
    {
        VNX::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        auto& projects = VNX::ProjectManager::instance();
        if (projects.currentProject() != nullptr)
            projects.closeProject();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        VNX::AssetRegistry::instance().clear();
    }
};

struct CurrentWorldSyncStateReset {
    ~CurrentWorldSyncStateReset()
    {
        HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
    }
};

float takeMasterPeak()
{
    VnPreviewChannelPeaksV1 peaks{};
    peaks.structSize = sizeof(peaks);
    if (VnPreview_TakeChannelPeaks(&peaks) != VN_OK)
        return 0.0f;
    float peak = 0.0f;
    for (uint32_t bus = 0; bus < peaks.busCount; ++bus) {
        if (peaks.busIds[bus] != VN_BUS_MASTER)
            continue;
        for (uint32_t channel = 0; channel < peaks.mixChannelCount; ++channel)
            peak = std::max(peak, peaks.channelPeaks[bus * VN_MAX_CHANNELS + channel]);
    }
    return peak;
}

float pumpUntilPreviewAudible(uint32_t maxBlocks = 32)
{
    if (VnPreview_EnableChannelPeakMetering() != VN_OK)
        return 0.0f;
    takeMasterPeak();
    for (uint32_t block = 0; block < maxBlocks; ++block) {
        if (Vn_Update() != VN_OK)
            return 0.0f;
        VnPreview_PumpNullMixer(1);
        const float peak = takeMasterPeak();
        if (peak > 0.0f)
            return peak;
    }
    return 0.0f;
}

}

TEST(AudioEnginePreview, ListenerStateChangesOnlyAfterAcceptedWritesAndResetsWithRuntime)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    float x = 0, y = 0, z = 0, fx = 0, fy = 0, fz = 0;
    ASSERT_TRUE(engine.getListenerInfo(1, &x, &y, &z, &fx, &fy, &fz));
    EXPECT_FLOAT_EQ(-1.0f, fz);
    EXPECT_FALSE(engine.getListenerInfo(0, &x, &y, &z, &fx, &fy, &fz));
    ASSERT_TRUE(engine.setListenerPositionById(1, 2, 3, 4, 1, 0, 0, 0, 1, 0));
    EXPECT_FALSE(engine.setListenerPositionById(0, 5, 6, 7, 1, 0, 0, 0, 1, 0));
    EXPECT_FALSE(engine.setListenerTransform({5, 6, 7}, {0, 0, 0}, {0, 1, 0}));
    ASSERT_TRUE(engine.getListenerInfo(1, &x, &y, &z, &fx, &fy, &fz));
    EXPECT_FLOAT_EQ(2.0f, x);
    EXPECT_FLOAT_EQ(3.0f, y);
    EXPECT_FLOAT_EQ(4.0f, z);
    EXPECT_FLOAT_EQ(1.0f, fx);
    EXPECT_FLOAT_EQ(0.0f, fz);
    engine.shutdown();
    EXPECT_FALSE(engine.getListenerInfo(1, &x, &y, &z, &fx, &fy, &fz));
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.getListenerInfo(1, &x, &y, &z, &fx, &fy, &fz));
    EXPECT_FLOAT_EQ(0.0f, x);
    EXPECT_FLOAT_EQ(0.0f, fx);
    EXPECT_FLOAT_EQ(-1.0f, fz);
}

TEST(AudioEnginePreview,
     RuntimeLifecycleCancelsPendingWorldBeforeTermAndAdvancesEpochOnRestart)
{
    ensureApp();
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    HarkenEditor::Bridge::setCurrentWorldDiscardObserverForTesting(
        observePendingDiscard);
    pendingDiscardObserved = false;
    runtimeActiveDuringPendingDiscard = false;
    discardedPendingToken = {};

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    auto& coordinator = engine.runtimeSyncCoordinator();
    const auto firstEpoch = coordinator.barrier(0).runtimeEpoch;
    ASSERT_NE(0u, firstEpoch);
    ASSERT_TRUE(engine.startOutput());

    const HarkenEditor::Bridge::CurrentWorldSyncToken token{31, 32, firstEpoch, 33};
    auto request = makeValidCurrentWorldRequest();
    ASSERT_TRUE(HarkenEditor::Bridge::syncCurrentWorld(
                    std::move(request), token).pending());

    engine.stopOutput();

    EXPECT_TRUE(pendingDiscardObserved);
    EXPECT_TRUE(runtimeActiveDuringPendingDiscard);
    EXPECT_EQ(token, discardedPendingToken);
    EXPECT_EQ(VNX::RuntimeSyncCoordinator::RevisionBarrierState::RuntimeUnavailable,
              coordinator.barrier(0).state);

    ASSERT_TRUE(engine.startOutput());
    EXPECT_GT(coordinator.barrier(0).runtimeEpoch, firstEpoch);

    engine.shutdown();
}

TEST(AudioEnginePreview,
     ShutdownSettlesBackpressuredProjectCloseBeforeRuntimeTeardown)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("BackpressuredCloseShutdown");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    ASSERT_TRUE(projects.closeProject());
    ASSERT_TRUE(projects.isClosing());
    ASSERT_TRUE(projects.hasPendingDestruction());

    engine.shutdown();

    EXPECT_FALSE(projects.isClosing());
    EXPECT_FALSE(projects.hasPendingDestruction());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_FALSE(projects.isClosing());
    EXPECT_FALSE(projects.hasPendingDestruction());
}

TEST(AudioEnginePreview, ClosedAsyncBootstrapCannotPublishRuntimePrepared)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    if (projects.currentProject() != nullptr)
        projects.closeProject();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("CancelledAsyncBootstrap");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    ASSERT_NE(projects.createItem(
                  nullptr, QStringLiteral("Layer"),
                  VNX::ProjectItem::Type::Layer),
              nullptr);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    bool finished = false;
    VNX::ProjectManager::syncProjectToRuntimeAsync(
        projects.currentProject(), &projects, [&finished]() {
            finished = true;
        });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_FALSE(finished);
    EXPECT_FALSE(projects.runtimePrepared());

    ASSERT_TRUE(projects.closeProject());
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    EXPECT_FALSE(finished);
    EXPECT_FALSE(projects.runtimePrepared());
    EXPECT_EQ(projects.currentProject(), nullptr);
    EXPECT_FALSE(projects.hasPendingDestruction());
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
}

namespace {

class AudioPlaybackRegression : public testing::Test {
protected:
    VNX::AudioEngine& engine = VNX::AudioEngine::instance();
    VNX::ProjectManager& projects = VNX::ProjectManager::instance();
    QTemporaryDir directory;
    VNX::ProjectItem* layer = nullptr;
    std::array<VNX::ProjectItem*, 2> sounds{};
    std::array<QString, 2> paths{};
    VNX::ProjectItem* action = nullptr;

    bool serviceUntilCurrentWorldReady(VNX::AudioEngine& target)
    {
        QElapsedTimer timeout;
        timeout.start();
        while (timeout.elapsed() < 5000) {
            QMetaObject::invokeMethod(&target, "onRuntimeServiceTimer", Qt::DirectConnection);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            target.runtimeSyncCoordinator().flushPendingMutationNow();
            const auto& coordinator = target.runtimeSyncCoordinator();
            const auto barrier = coordinator.barrier(coordinator.currentAuthoringRevision());
            if (barrier.state == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready)
                return true;
            if (barrier.state != VNX::RuntimeSyncCoordinator::RevisionBarrierState::Pending) {
                ADD_FAILURE() << "World state=" << static_cast<int>(barrier.state)
                              << " failure=" << static_cast<int>(barrier.failure);
                return false;
            }
            VnPreview_PumpNullMixer(1);
            QThread::msleep(1);
        }
        return false;
    }

    void SetUp() override
    {
        ensureApp();
        engine.shutdown();
        if (projects.currentProject())
            projects.closeProject();
        QCoreApplication::processEvents();
        HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
        VNX::AssetRegistry::instance().clear();
        ASSERT_TRUE(directory.isValid());
        HarkenEditor::Bridge::EngineSessionConfig config;
        config.sampleRate = 48000;
        config.bufferSize = 64;
        config.maxVoices = 16;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(engine.initialize(config));
        ASSERT_TRUE(engine.startOutput());
        VNX::ProjectConfig projectConfig;
        projectConfig.name = QStringLiteral("PlaybackRegression");
        projectConfig.location = directory.path();
        ASSERT_TRUE(projects.createProject(projectConfig));
        layer = projects.createItem(nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        const QDir audioRoot(QFileInfo(projects.currentProject()->projectPath).absolutePath()
                             + QStringLiteral("/AudioSources"));
        for (int index = 0; index < 2; ++index) {
            const QString name = QStringLiteral("Sound%1").arg(index);
            paths[index] = audioRoot.filePath(name + QStringLiteral(".wav"));
            writeWavF32(paths[index], std::vector<float>(65536, 0.125f * (index + 1)));
            VNX::AssetInfo asset;
            asset.sourceID = 930001 + index;
            asset.relativePath = name + QStringLiteral(".wav");
            asset.absolutePath = paths[index];
            asset.metadata.sampleRate = 48000;
            asset.metadata.channels = 1;
            asset.metadata.bitDepth = 32;
            asset.metadata.totalSamples = 65536;
            asset.metadata.durationSec = 65536.0 / 48000.0;
            asset.metadata.fileSize = static_cast<uint64_t>(QFileInfo(paths[index]).size());
            asset.contentHash = VNX::AssetImporter::computeContentHash(paths[index]);
            ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
            sounds[index] = projects.createAudioItem(layer, name, paths[index], asset.sourceID);
            ASSERT_NE(sounds[index], nullptr);
        }
        action = projects.createActionTargeting(sounds[0], VNX::VActionStep::Type::Play,
                                                QStringLiteral("Play_Good"));
        ASSERT_NE(action, nullptr);
        VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
        ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    }

    void TearDown() override
    {
        engine.shutdown();
        QCoreApplication::processEvents();
        if (projects.currentProject())
            projects.closeProject();
        QCoreApplication::processEvents();
        HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
        VNX::AssetRegistry::instance().clear();
    }

    void expectGoodAction()
    {
        const auto result = engine.playByNameDetailed(QStringLiteral("Play_Good"));
        ASSERT_EQ(result.engineResult, HarkenEditor::Bridge::kEngineOk);
        ASSERT_NE(result.playbackHandle, 0u);
        EXPECT_GT(pumpUntilPreviewAudible(64), 0.05f);
    }

    float settledPeak()
    {
        if (VnPreview_EnableChannelPeakMetering() != VN_OK)
            return 0.0f;
        for (int block = 0; block < 31; ++block) {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            VnPreview_PumpNullMixer(1);
        }
        QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        takeMasterPeak();
        VnPreview_PumpNullMixer(1);
        return takeMasterPeak();
    }
};

TEST_F(AudioPlaybackRegression, ConcurrentPreviewCancelAndUndoRemainConsistentForCurrentAndFuturePlayback)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();
    const auto started = engine.playByNameDetailed(QStringLiteral("Play_Good"));
    ASSERT_EQ(started.engineResult, HarkenEditor::Bridge::kEngineOk);
    const float baseline = settledPeak();
    ASSERT_GT(baseline, 0.05f);
    VNX::PropertyEditSession parentPreview(layer, QStringLiteral("volume"), 0.0);
    parentPreview.preview(-6.0);
    VNX::PropertyEditSession soundEdit(sounds[0], QStringLiteral("volume"), 0.0);
    soundEdit.preview(-6.0);
    EXPECT_NEAR(settledPeak(), baseline * std::pow(10.0f, -12.0f / 20.0f), 0.002f);
    soundEdit.commit();
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    parentPreview.cancel();
    EXPECT_NEAR(settledPeak(), baseline * std::pow(10.0f, -6.0f / 20.0f), 0.002f);

    VNX::PropertyEditSession pitchEdit(sounds[0], QStringLiteral("pitch"), 0.0);
    pitchEdit.preview(100.0);
    pitchEdit.commit();
    EXPECT_NEAR(settledPeak(), baseline * std::pow(10.0f, -6.0f / 20.0f), 0.002f);
    engine.stopAll(0);
    settledPeak();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    const auto scalarPeak = settledPeak();
    EXPECT_NEAR(scalarPeak, baseline * std::pow(10.0f, -6.0f / 20.0f), 0.002f);
    auto& coordinator = engine.runtimeSyncCoordinator();
    coordinator.markCurrentWorldMutation();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_NEAR(settledPeak(), scalarPeak, 0.002f);

    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_NEAR(settledPeak(), scalarPeak, 0.002f);
    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_NEAR(settledPeak(), baseline, 0.002f);
    engine.stopAll(0);
    settledPeak();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    EXPECT_NEAR(settledPeak(), baseline, 0.002f);
}

TEST_F(AudioPlaybackRegression, ScalarPublicationRechecksSourceFilesAndLostRuntimeRegistration)
{
    namespace Bridge = HarkenEditor::Bridge;
    auto& coordinator = engine.runtimeSyncCoordinator();
    ASSERT_TRUE(QFile::remove(paths[1]));
    coordinator.markCurrentWorldPropertyMutation(sounds[0]->runtimeId(), 1);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_FALSE(Bridge::runtimeObjectExists(sounds[1]->runtimeId()));
    writeWavF32(paths[1], std::vector<float>(65536, 0.25f));
    coordinator.markCurrentWorldPropertyMutation(sounds[0]->runtimeId(), 1);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_TRUE(Bridge::runtimeObjectExists(sounds[1]->runtimeId()));
    coordinator.markCurrentWorldMutation();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto revision = Bridge::registeredSoundRevision(sounds[0]->runtimeId());
    ASSERT_NE(revision, 0u);
    Bridge::removeRuntimeObject(sounds[0]->runtimeId());
    coordinator.markCurrentWorldPropertyMutation(sounds[0]->runtimeId(), 1);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(Bridge::registeredSoundRevision(sounds[0]->runtimeId()), revision);
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, RuntimeRestartCannotAdoptAnOlderCompilation)
{
    auto& coordinator = engine.runtimeSyncCoordinator();
    coordinator.markCurrentWorldPropertyMutation(sounds[0]->runtimeId(), 1);
    coordinator.flushPendingMutationNow();
    const auto previous = coordinator.barrier(coordinator.currentAuthoringRevision()).runtimeEpoch;
    engine.stopOutput();
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto barrier = coordinator.barrier(coordinator.currentAuthoringRevision());
    EXPECT_GT(barrier.runtimeEpoch, previous);
    EXPECT_EQ(coordinator.installedWorld()->runtimeEpoch, barrier.runtimeEpoch);
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, UnchangedSourceRefreshRetriesFailedWorld)
{
    const auto waitForMedia = qScopeGuard([] { QThreadPool::globalInstance()->waitForDone(); });
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(930001, paths[0]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    QThreadPool::globalInstance()->waitForDone();
    const auto stepType = action->comp<VNX::VAction>()->steps.front().type;
    action->comp<VNX::VAction>()->steps.front().type = static_cast<VNX::VActionStep::Type>(255);
    auto& coordinator = engine.runtimeSyncCoordinator();
    coordinator.markCurrentWorldMutation();
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);

    action->comp<VNX::VAction>()->steps.front().type = stepType;
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(930001, paths[0]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    expectGoodAction();
}

class StudioActionSeekTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        engine.shutdown();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (projects.currentProject() != nullptr)
            projects.closeProject();
        VNX::AssetRegistry::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();

        HarkenEditor::Bridge::EngineSessionConfig config;
        config.sampleRate = 48000;
        config.bufferSize = 64;
        config.maxVoices = 16;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(engine.initialize(config));
        ASSERT_TRUE(engine.startOutput());
        ASSERT_TRUE(directory.isValid());

        VNX::ProjectConfig projectConfig;
        projectConfig.name = QStringLiteral("StudioActionSeek");
        projectConfig.location = directory.path();
        ASSERT_TRUE(projects.createProject(projectConfig));
        auto* project = projects.currentProject();
        ASSERT_NE(project, nullptr);

        const QString audioDirectory = QDir(
            QFileInfo(project->projectPath).absolutePath())
                .filePath(QStringLiteral("AudioSources"));
        ASSERT_TRUE(QDir().mkpath(audioDirectory));
        wavPath = QDir(audioDirectory).filePath(
            QStringLiteral("seek.wav"));
        std::vector<float> samples(131072, -0.25f);
        std::fill(samples.begin() + 1024, samples.end(), 0.5f);
        writeWavF32(wavPath, samples);

        VNX::AssetInfo asset;
        asset.sourceID = 7901;
        asset.relativePath = QStringLiteral("seek.wav");
        asset.absolutePath = wavPath;
        asset.metadata.sampleRate = 48000;
        asset.metadata.channels = 1;
        asset.metadata.totalSamples = samples.size();
        asset.metadata.durationSec =
            static_cast<double>(samples.size()) / 48000.0;
        asset.metadata.fileSize =
            static_cast<uint64_t>(QFileInfo(wavPath).size());
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));

        auto* layer = projects.createItem(
            nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
        sound = projects.createAudioItem(
            layer, QStringLiteral("SeekSound"), wavPath, asset.sourceID);
        action = projects.createItem(
            nullptr, QStringLiteral("SeekAction"),
            VNX::ProjectItem::Type::Action);
        ASSERT_NE(layer, nullptr);
        ASSERT_NE(sound, nullptr);
        ASSERT_NE(action, nullptr);
        VNX::VActionStep play;
        play.targetObjectID = sound->objectID;
        action->comp<VNX::VAction>()->steps = {play};

        VNX::ProjectManager::syncProjectToRuntime(project);
        ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
        VNX::registerAdvancedActions(
            VNX::ActionManager::instance(), &engine);
    }

    void TearDown() override
    {
        VNX::ActionManager::instance().clearAllForTesting();
        engine.shutdown();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (projects.currentProject() != nullptr)
            projects.closeProject();
        VNX::AssetRegistry::instance().clear();
    }

    VNX::AudioEngine& engine = VNX::AudioEngine::instance();
    VNX::ProjectManager& projects = VNX::ProjectManager::instance();
    QTemporaryDir directory;
    QString wavPath;
    VNX::ProjectItem* sound = nullptr;
    VNX::ProjectItem* action = nullptr;
};

TEST_F(StudioActionSeekTest, CommandDispatchRequiresCurrentWorldAndMatchingProjectSession)
{
    int executions = 0;
    VNX::ActionDef command;
    command.id = QStringLiteral("post_action");
    command.handler = [&](const QJsonObject&) -> VNX::ActionResult {
        ++executions;
        return {true, {}};
    };
    VNX::ActionManager::instance().registerAction(command);
    VNX::IPCServer dispatcher(&engine);
    dispatcher.setProjectManager(&projects);
    dispatcher.setAppState(VNX::IPCServer::AppState::Ready);
    auto& coordinator = engine.runtimeSyncCoordinator();
    coordinator.beginSubmissionBatch();
    coordinator.markCurrentWorldMutation();
    const QJsonObject args{{QStringLiteral("name"), action->name}};
    const auto pending = dispatcher.dispatchCommand(QStringLiteral("post_action"), args);
    coordinator.endSubmissionBatch();
    EXPECT_EQ(pending.value(QStringLiteral("code")).toString(), QStringLiteral("RUNTIME_SYNC_PENDING"));
    EXPECT_EQ(executions, 0);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    VNX::IPCServer::IPCDispatchContext context;
    context.expectedProjectSession = QStringLiteral("different-session");
    const auto changed = dispatcher.dispatchCommand(QStringLiteral("post_action"), args, context);
    EXPECT_EQ(changed.value(QStringLiteral("code")).toString(), QStringLiteral("PROJECT_CHANGED"));
    EXPECT_EQ(executions, 0);
    context.expectedProjectSession = pending.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("project_session_id")).toString();
    const auto ready = dispatcher.dispatchCommand(QStringLiteral("post_action"), args, context);
    EXPECT_EQ(ready.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(executions, 1);
}

}
