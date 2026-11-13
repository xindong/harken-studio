#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Engine.h"
#include "AudioEngine.h"
#include "CompileTargetProfileCatalog.h"
#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/MediaPool.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Components/VAction.h"
#include "Components/VTrack.h"
#include "Components/VEffect.h"
#include "Components/VMixNode.h"
#include "PlatformContextService.h"
#include "Widgets/EffectEditor/EffectEditorPanel.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Components/VAudio.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "PropertyEditSession.h"
#include "UndoCommands.h"
#include "RuntimeSyncCoordinator.h"
#include "RuntimeCapabilityMatrix.h"
#include "AuditionManager.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/ProjectSyncBridge.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "RuntimeBridge/PreviewBridge.h"
#include "RuntimeBridge/RuntimeLink.h"
#include "harken/Harken.h"
#include <harken_bus_compiler/BusCompiler.h>

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

harkenstudiobuild::WorldInstallResult backpressuredWorldInstall(
    const harkenstudiobuild::ContainerWorldInstallViewV1&) noexcept
{
    harkenstudiobuild::WorldInstallResult result;
    result.status = harkenstudiobuild::WorldInstallStatus::Backpressure;
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
    auto* runtime = harken::detail::ActiveEngine();
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < 5000) {
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

float pumpUntilPreviewAudible(uint32_t maxBlocks = 32)
{
    harken::Engine* runtime = harken::detail::ActiveEngine();
    if (runtime == nullptr)
        return 0.0f;
    for (uint32_t block = 0; block < maxBlocks; ++block) {
        if (Vn_Update() != VN_OK)
            return 0.0f;
        runtime->nullBackend.Pump(1);
        const float* output = runtime->nullBackend.LastBlock();
        const uint32_t sampleCount = runtime->nullBackend.BlockFrames()
            * runtime->nullBackend.Channels();
        float peak = 0.0f;
        for (uint32_t sample = 0; sample < sampleCount; ++sample)
            peak = std::max(peak, std::abs(output[sample]));
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

TEST(AudioEnginePreview, RuntimeRestartCancelsPendingPreviewStart)
{
    ensureApp();
    AudioEnginePreviewReset reset;
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

    VNX::IPCServer ipc(&engine);
    ipc.setProjectManager(&projects);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("ContainerMoveBackpressure");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));

    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* first = projects.createItem(
        layer, QStringLiteral("First"), VNX::ProjectItem::Type::Container);
    auto* second = projects.createItem(
        layer, QStringLiteral("Second"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    std::vector<VNX::ProjectItem*> sounds;
    const QString audioSourcesPath = QDir(
        QFileInfo(projects.currentProject()->projectPath).absolutePath())
            .filePath(QStringLiteral("AudioSources"));
    for (uint32_t index = 0; index < 3; ++index) {
        const QString fileName = QStringLiteral("move%1.wav").arg(index + 1);
        const QString wavPath = QDir(audioSourcesPath).filePath(fileName);
        writeWavPcm24(wavPath, 48000);

        VNX::AssetInfo asset;
        asset.sourceID = 7201 + index;
        asset.relativePath = fileName;
        asset.absolutePath = wavPath;
        asset.metadata.sampleRate = 48000;
        asset.metadata.channels = 2;
        asset.metadata.bitDepth = 24;
        asset.metadata.totalSamples = 48000;
        asset.metadata.durationSec = 1.0;
        asset.metadata.fileSize = static_cast<uint64_t>(QFileInfo(wavPath).size());
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
        auto* sound = projects.createAudioItem(
            first, QStringLiteral("Sound%1").arg(index + 1),
            wavPath, asset.sourceID);
        ASSERT_NE(sound, nullptr);
        sounds.push_back(sound);
    }

    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    for (auto* sound : sounds)
        ASSERT_TRUE(projects.moveItem(sound, second));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    int startedCount = 0;
    int failedCount = 0;
    uint32_t failedPlay = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&startedCount](uint32_t, const QString&) {
            ++startedCount;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&](uint32_t playingId, const QString&, int32_t) {
            ++failedCount;
            failedPlay = playingId;
        });
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    uint32_t secondPlay = 0;
    {
        QScopedValueRollback admissionClosed(
            runtime->currentWorldReplacementAdmissionClosed, true);
        secondPlay = audition.playItem(*second);
    }
    ASSERT_NE(secondPlay, 0u);
    ASSERT_TRUE(audition.lastPlayPending());
    ASSERT_TRUE(audition.getAudition(secondPlay).previewStartPending);
    EXPECT_EQ(audition.getAudition(secondPlay).playbackHandle, 0u);
    EXPECT_EQ(startedCount, 0);
    EXPECT_EQ(failedCount, 0);
    EXPECT_EQ(audition.activeAuditionCount(), 1);

    engine.shutdown();
    EXPECT_EQ(failedCount, 1);
    EXPECT_EQ(failedPlay, secondPlay);
    EXPECT_EQ(startedCount, 0);
    EXPECT_EQ(audition.activeAuditionCount(), 0);

    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &audition, "retryPendingPreviewStarts", Qt::DirectConnection));
    EXPECT_EQ(startedCount, 0);
    EXPECT_EQ(failedCount, 1);
    EXPECT_EQ(audition.activeAuditionCount(), 0);

    const uint32_t newPlay = audition.playItem(*second);
    ASSERT_NE(newPlay, 0u);
    EXPECT_NE(newPlay, secondPlay);
    EXPECT_FALSE(audition.lastPlayPending());
    EXPECT_NE(audition.getAudition(newPlay).playbackHandle, 0u);
    EXPECT_EQ(startedCount, 1);
    EXPECT_EQ(failedCount, 1);
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
            harken::detail::ActiveEngine()->nullBackend.Pump(1);
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
        auto* runtime = harken::detail::ActiveEngine();
        for (int block = 0; block < 32; ++block) {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            runtime->nullBackend.Pump(1);
        }
        const float* output = runtime->nullBackend.LastBlock();
        const auto size = runtime->nullBackend.BlockFrames() * runtime->nullBackend.Channels();
        float peak = 0.0f;
        for (std::uint32_t index = 0; index < size; ++index)
            peak = std::max(peak, std::abs(output[index]));
        return peak;
    }
};

TEST_F(AudioPlaybackRegression, EffectEditorDragPreviewsBeforeCommitAndCancelsWithoutUndo)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    VNX::EffectEntry entry;
    entry.guid = QUuid::createUuid();
    entry.mode = VNX::EffectMode::Custom;
    entry.custom.parameters = {0.0f, 20.0f};
    VNX::EffectFieldGroup group;
    ASSERT_TRUE(group.effects.replace({entry}));
    sounds[0]->fieldGroups.effects.shared = {VNX::FieldGroupMode::Local, group};
    projects.itemPropertyChanged(sounds[0], QStringLiteral("fieldGroups"), nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
        HarkenEditor::Bridge::kEngineOk);
    const auto dry = settledPeak();
    VNX::EffectEditorPanel panel;
    panel.bindEffect(sounds[0]->guid, entry.guid);
    auto* slider = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("effectParameter_ceilingDb"));
    ASSERT_NE(slider, nullptr);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-24);
    EXPECT_LT(settledPeak(), dry * 0.6f);
    EXPECT_EQ(0, undo->count());
    slider->binding()->cancelEdit();
    settledPeak();
    settledPeak();
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    EXPECT_EQ(0, undo->count());
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-21);
    slider->binding()->previewEdit(-24);
    EXPECT_LT(settledPeak(), dry * 0.6f);
    slider->binding()->commitEdit();
    EXPECT_EQ(1, undo->count());
    EXPECT_LT(settledPeak(), dry * 0.6f);
    undo->undo();
    settledPeak();
    settledPeak();
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
}

TEST_F(AudioPlaybackRegression, EffectEditorPresetDragRestoresOnSelectionChange)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    auto* preset = projects.createItem(nullptr, QStringLiteral("DragPreset"), VNX::ProjectItem::Type::Effect);
    ASSERT_NE(preset, nullptr);
    auto* effect = preset->comp<VNX::VEffect>();
    ASSERT_NE(effect, nullptr);
    const auto original = effect->definition.parameters[0];
    VNX::EffectEditorPanel panel;
    panel.bindEffect(preset->guid);
    auto* slider = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("effectParameter_ceilingDb"));
    ASSERT_NE(slider, nullptr);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-24);
    EXPECT_FLOAT_EQ(-24, effect->definition.parameters[0]);
    EXPECT_EQ(0, undo->count());
    panel.clearEditor();
    EXPECT_FLOAT_EQ(original, effect->definition.parameters[0]);
    EXPECT_EQ(0, undo->count());
}

TEST_F(AudioPlaybackRegression, EffectEditorExternalPresetEditEndsDragBeforeTakingUndoSnapshot)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    auto* preset = projects.createItem(nullptr, QStringLiteral("ExternalPreset"), VNX::ProjectItem::Type::Effect);
    ASSERT_NE(preset, nullptr);
    auto* effect = preset->comp<VNX::VEffect>();
    const auto original = effect->definition.parameters[0];
    VNX::EffectEditorPanel panel;
    panel.bindEffect(preset->guid);
    auto* slider = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("effectParameter_ceilingDb"));
    ASSERT_NE(slider, nullptr);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-24);
    ASSERT_TRUE(VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(preset->objectID)},
         {QStringLiteral("property"), QStringLiteral("ceilingDb")},
         {QStringLiteral("value"), -12.0}}, VNX::ActionSource::IPC).success);
    QCoreApplication::processEvents();
    EXPECT_FLOAT_EQ(-12, effect->definition.parameters[0]);
    slider->binding()->previewEdit(-30);
    slider->binding()->commitEdit();
    EXPECT_FLOAT_EQ(-12, effect->definition.parameters[0]);
    EXPECT_EQ(1, undo->count());
    undo->undo();
    EXPECT_FLOAT_EQ(original, effect->definition.parameters[0]);
    QCoreApplication::processEvents();
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-18);
    projects.itemCreated(preset);
    EXPECT_FLOAT_EQ(original, effect->definition.parameters[0]);
    EXPECT_DOUBLE_EQ(original, slider->value());
}

TEST_F(AudioPlaybackRegression, EffectEditorBusDragCannotRestoreADeletedPresetReference)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    VNX::PlatformContextService::instance().setAll(true);
    for (const bool overlay : {false, true}) {
    SCOPED_TRACE(overlay);
    auto* preset = projects.createItem(nullptr, QStringLiteral("DeletedPreset"), VNX::ProjectItem::Type::Effect);
    auto* bus = projects.createItem(projects.getMasterBusItem(), QStringLiteral("DragBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(preset, nullptr);
    ASSERT_NE(bus, nullptr);
    VNX::EffectEntry custom;
    custom.guid = QUuid::createUuid();
    custom.mode = VNX::EffectMode::Custom;
    custom.custom.parameters = {0.0f, 20.0f};
    VNX::EffectEntry reference;
    reference.guid = QUuid::createUuid();
    reference.effectGuid = preset->guid;
    auto* mix = bus->comp<VNX::VMixNode>();
    const auto platform = projects.currentProject()->platforms.front().id;
    ASSERT_TRUE(mix->effects.replace(overlay ? std::vector<VNX::EffectEntry>{custom}
        : std::vector<VNX::EffectEntry>{custom, reference}));
    if (overlay) ASSERT_TRUE(mix->effectPlatforms[platform].replace({custom, reference}));
    VNX::EffectEditorPanel panel;
    panel.bindEffect(bus->guid, custom.guid);
    auto* slider = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("effectParameter_ceilingDb"));
    ASSERT_NE(slider, nullptr);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-24);
    EXPECT_FLOAT_EQ(-24, mix->effects.entries()[0].custom.parameters[0]);
    ASSERT_TRUE(projects.deleteItem(preset));
    QCoreApplication::processEvents();
    ASSERT_EQ(1u, mix->effects.entries().size());
    if (overlay) {
        ASSERT_TRUE(mix->effectPlatforms.contains(platform));
        EXPECT_EQ(1u, mix->effectPlatforms.at(platform).entries().size());
    }
    EXPECT_FLOAT_EQ(0, mix->effects.entries()[0].custom.parameters[0]);
    EXPECT_EQ(1, undo->count());
    undo->undo();
    const auto& restored = overlay ? mix->effectPlatforms.at(platform) : mix->effects;
    ASSERT_EQ(2u, restored.entries().size());
    EXPECT_FLOAT_EQ(0, restored.entries()[0].custom.parameters[0]);
    EXPECT_EQ(reference.effectGuid, restored.entries()[1].effectGuid);
    }
}

class AuditionLifecycleTest : public AudioPlaybackRegression {
protected:
    HarkenEditor::AuditionManager audition{&engine};

    void makeLooping()
    {
        for (auto* sound : sounds)
            sound->comp<VNX::VAudio>()->loopCount = 0;
        VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
        ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    }

    void poll()
    {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "pollAuditionStates", Qt::DirectConnection));
    }

    float pump(uint32_t blocks)
    {
        auto* runtime = harken::detail::ActiveEngine();
        float peak = 0.0f;
        for (uint32_t block = 0; block < blocks; ++block) {
            engine.serviceRuntime();
            runtime->nullBackend.Pump(1);
            const auto count = runtime->nullBackend.BlockFrames() * runtime->nullBackend.Channels();
            const float* output = runtime->nullBackend.LastBlock();
            for (uint32_t sample = 0; sample < count; ++sample)
                peak = std::max(peak, std::abs(output[sample]));
            engine.serviceRuntime();
            poll();
        }
        return peak;
    }

    void fillCommands()
    {
        auto* runtime = harken::detail::ActiveEngine();
        harken::CommandRecord filler{};
        filler.opcode = harken::kOpcodeStop;
        filler.version = harken::kCommandVersion;
        filler.payload.stop.handle = 1;
        while (runtime->commands.FreeSlots() != 0)
            ASSERT_TRUE(runtime->commands.TryPush(filler));
    }
};

TEST_F(AuditionLifecycleTest, SilentScheduledTreesRemainControllableUntilTheyReallyEnd)
{
    makeLooping();
    action->comp<VNX::VAction>()->steps.front().delay = 2000;
    auto* parent = projects.createActionTargeting(
        action, VNX::VActionStep::Type::PostAction, QStringLiteral("Parent"));
    ASSERT_NE(parent, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    const auto first = audition.playItem(*action);
    const auto second = audition.playItem(*parent);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    EXPECT_FLOAT_EQ(pump(400), 0.0f);
    EXPECT_EQ(audition.activeAuditionCount(), 2);
    EXPECT_TRUE(audition.hasActiveNonRawAudition());

    audition.stopAll(100);
    EXPECT_FLOAT_EQ(pump(2000), 0.0f);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
    audition.stopAll(100);
    EXPECT_FLOAT_EQ(pump(2000), 0.0f);
}

TEST_F(AuditionLifecycleTest, CompletionAndToolbarScopeAreIndependentOfOtherAudio)
{
    const auto rawSound = engine.mintRawPcmSoundId();
    const auto registered = engine.registerRawPcmSlice(
        rawSound, std::vector<float>(48000, 0.25f), 1, 48000, 0, 48000, true);
    ASSERT_EQ(registered.result, HarkenEditor::Bridge::kEngineOk);
    const auto rawHandle = engine.playRawPcm(rawSound, true);
    ASSERT_NE(rawHandle, 0u);
    const auto raw = audition.registerRawFilePlayback(QStringLiteral("Raw"), rawHandle);
    ASSERT_NE(audition.playItem(*action), 0u);
    EXPECT_GT(pump(1800), 0.05f);
    EXPECT_EQ(audition.activeAuditionCount(), 1);
    EXPECT_EQ(audition.getAudition(raw).playbackHandle, rawHandle);
    EXPECT_FALSE(audition.hasActiveNonRawAudition());

    ASSERT_NE(audition.playItem(*action), 0u);
    pump(32);
    audition.stopAll(100);
    pump(160);
    EXPECT_EQ(audition.activeAuditionCount(), 1);
    EXPECT_GT(pump(100), 0.05f);
    audition.stop(raw);
    pump(32);
    EXPECT_FLOAT_EQ(pump(100), 0.0f);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
    EXPECT_EQ(engine.unregisterRawPcm(rawSound), HarkenEditor::Bridge::kEngineOk);
}

TEST_F(AuditionLifecycleTest, StopCompletionFollowsAudioAndRepeatedRequestsDoNotExtendFades)
{
    makeLooping();
    const auto first = audition.playItem(*action);
    const auto second = audition.playItem(*action);
    const auto preview = audition.playItem(*sounds[1]);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    ASSERT_NE(preview, 0u);
    EXPECT_GT(pump(32), 0.05f);
    EXPECT_EQ(audition.getActiveAuditions().size(), 3);
    int completed = 0;
    QObject::connect(&audition, &HarkenEditor::AuditionManager::allAuditionsStopped,
                     &audition, [&] { ++completed; });

    audition.stopAll(100);
    EXPECT_EQ(completed, 0);
    EXPECT_EQ(audition.activeAuditionCount(), 3);
    EXPECT_EQ(audition.stopIpcSnapshot().value("state").toString(), "stopping");
    EXPECT_GT(pump(16), 0.01f);
    EXPECT_EQ(audition.activeAuditionCount(), 3);
    for (int attempt = 0; attempt < 16; ++attempt) {
        audition.stopAll(100);
        pump(10);
    }
    EXPECT_EQ(audition.activeAuditionCount(), 0);
    EXPECT_EQ(completed, 1);
    EXPECT_FLOAT_EQ(pump(1500), 0.0f);
}

TEST_F(AuditionLifecycleTest, TemporaryRefusalRetriesWithoutReportingCompletion)
{
    makeLooping();
    for (float fade : {0.0f, 100.0f}) {
        SCOPED_TRACE(fade);
        ASSERT_NE(audition.playItem(*action), 0u);
        ASSERT_NE(audition.playItem(*sounds[1]), 0u);
        EXPECT_GT(pump(32), 0.05f);
        fillCommands();
        audition.stopAll(fade);
        EXPECT_EQ(audition.activeAuditionCount(), 2);
        for (const auto& info : audition.getActiveAuditions()) {
            EXPECT_EQ(info.stopResult, HarkenEditor::Bridge::kEngineBackpressure);
            EXPECT_EQ(info.stopState, HarkenEditor::AuditionStopState::Pending);
        }
        harken::detail::ActiveEngine()->nullBackend.Pump(1);
        poll();
        pump(160);
        EXPECT_EQ(audition.activeAuditionCount(), 0);
        EXPECT_FLOAT_EQ(pump(100), 0.0f);
    }
}

TEST_F(AuditionLifecycleTest, ReplacingAStoppingPreviewKeepsTheNewPlaybackControllable)
{
    makeLooping();
    const auto id = audition.playItem(*sounds[1]);
    ASSERT_NE(id, 0u);
    EXPECT_GT(pump(32), 0.05f);
    audition.stop(id, 100);
    ASSERT_NE(audition.playItem(*sounds[1]), 0u);
    pump(160);
    EXPECT_EQ(audition.activeAuditionCount(), 1);
    EXPECT_GT(pump(32), 0.05f);
    audition.stopAll(100);
    pump(160);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
    EXPECT_FLOAT_EQ(pump(100), 0.0f);
}

TEST_F(AuditionLifecycleTest, PersistentRefusalIsBoundedAndPreservesManualControl)
{
    makeLooping();
    const auto id = audition.playItem(*action);
    ASSERT_NE(id, 0u);
    EXPECT_GT(pump(32), 0.05f);
    int failures = 0;
    QObject::connect(&audition, &HarkenEditor::AuditionManager::auditionStopFailed,
                     &audition, [&](uint32_t, const QString&, int32_t) { ++failures; });
    fillCommands();
    audition.stopAll();
    for (int attempt = 0; attempt < 100; ++attempt)
        poll();
    EXPECT_EQ(failures, 1);
    EXPECT_EQ(audition.activeAuditionCount(), 1);
    EXPECT_EQ(audition.stopIpcSnapshot().value("state").toString(), "stop_failed");
    const auto pressure = harken::detail::ActiveEngine()->commands.BackpressureCount();
    for (int attempt = 0; attempt < 100; ++attempt)
        poll();
    EXPECT_EQ(harken::detail::ActiveEngine()->commands.BackpressureCount(), pressure);
    EXPECT_GT(pump(32), 0.05f);
    audition.stop(id);
    pump(32);
    EXPECT_FLOAT_EQ(pump(100), 0.0f);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
}

TEST_F(AuditionLifecycleTest, RejectedStopKeepsItsResultAndCanBeRetried)
{
    makeLooping();
    const auto id = audition.playItem(*action);
    ASSERT_NE(id, 0u);
    EXPECT_GT(pump(32), 0.05f);
    {
        QScopedValueRollback refusal(harken::detail::ActiveEngine()->callbackFatalSticky, true);
        audition.stop(id);
        EXPECT_EQ(audition.activeAuditionCount(), 1);
        EXPECT_EQ(audition.getAudition(id).stopState, HarkenEditor::AuditionStopState::Failed);
        EXPECT_EQ(audition.getAudition(id).stopResult, HarkenEditor::Bridge::kEngineNotInit);
    }
    EXPECT_GT(pump(32), 0.05f);
    audition.stop(id);
    pump(32);
    EXPECT_FLOAT_EQ(pump(100), 0.0f);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
}

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

TEST_F(AudioPlaybackRegression, LimiterEditsPreserveRunningInstancesAndApplyStructureLive)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();
    VNX::EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.mode = VNX::EffectMode::Custom;
    effect.custom.parameters = {-24.0f, 1.0f};
    effect.bypass = true;
    const auto replace = [&](const QJsonArray& effects) {
        return VNX::ActionManager::instance().execute(QStringLiteral("replace_field_group_effects"),
            {{QStringLiteral("definition_guid"), layer->guid.toString(QUuid::WithoutBraces)},
             {QStringLiteral("group"), QStringLiteral("effect")},
             {QStringLiteral("scope"), QStringLiteral("Shared")},
             {QStringLiteral("effects"), effects}}, VNX::ActionSource::GUI).success;
    };
    ASSERT_TRUE(replace({effect.toJson()}));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto started = engine.playByNameDetailed(QStringLiteral("Play_Good"));
    ASSERT_EQ(started.engineResult, HarkenEditor::Bridge::kEngineOk);
    const float dry = settledPeak();
    ASSERT_GT(dry, 0.05f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> before;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(before));
    ASSERT_EQ(before.size(), 1u);

    effect.bypass = false;
    ASSERT_TRUE(replace({effect.toJson()}));
    const float limited = settledPeak();
    EXPECT_GT(limited, 0.0f);
    EXPECT_LT(limited, dry * 0.7f);
    effect.custom.parameters[0] = 0.0f;
    ASSERT_TRUE(replace({effect.toJson()}));
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_NEAR(settledPeak(), limited, 0.002f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> after;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(after));
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after.front().voiceInstance, before.front().voiceInstance);

    auto* preset = projects.createItem(nullptr, QStringLiteral("ReplacementLimiter"),
                                      VNX::ProjectItem::Type::Effect);
    ASSERT_NE(preset, nullptr);
    effect.mode = VNX::EffectMode::Preset;
    effect.effectGuid = preset->guid;
    ASSERT_TRUE(replace({effect.toJson()}));
    EXPECT_GT(settledPeak(), dry * 0.9f);
    effect.bypass = true;
    ASSERT_TRUE(replace({effect.toJson()}));
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    effect.bypass = false;
    ASSERT_TRUE(replace({effect.toJson()}));
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    engine.stopAll(0);
    settledPeak();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    EXPECT_NEAR(settledPeak(), dry, 0.002f);

    ASSERT_TRUE(VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(preset->objectID)},
         {QStringLiteral("property"), QStringLiteral("ceilingDb")},
         {QStringLiteral("value"), -24.0}}, VNX::ActionSource::GUI).success);
    EXPECT_NEAR(settledPeak(), limited, 0.002f);
    ASSERT_TRUE(replace({}));
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    engine.stopAll(0);
    settledPeak();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
}

TEST_F(AudioPlaybackRegression, LimiterSourceReplacementBeforeFirstLiveTickAdoptsLatestSource)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    auto* preset = projects.createItem(nullptr, QStringLiteral("FirstTickReplacement"),
                                      VNX::ProjectItem::Type::Effect);
    ASSERT_NE(preset, nullptr);
    VNX::EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.mode = VNX::EffectMode::Custom;
    effect.custom.parameters = {-24.0f, 1.0f};
    const auto replace = [&] {
        return VNX::ActionManager::instance().execute(QStringLiteral("replace_field_group_effects"),
            {{QStringLiteral("definition_guid"), layer->guid.toString(QUuid::WithoutBraces)},
             {QStringLiteral("group"), QStringLiteral("effect")},
             {QStringLiteral("scope"), QStringLiteral("Shared")},
             {QStringLiteral("effects"), QJsonArray{effect.toJson()}}}, VNX::ActionSource::GUI).success;
    };
    ASSERT_TRUE(replace());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    ASSERT_GT(pumpUntilPreviewAudible(), 0.0f);
    effect.mode = VNX::EffectMode::Preset;
    effect.effectGuid = preset->guid;
    ASSERT_TRUE(replace());
    const float limited = settledPeak();
    ASSERT_GT(limited, 0.0f);
    effect.bypass = true;
    ASSERT_TRUE(replace());
    const float dry = settledPeak();
    EXPECT_GT(limited, dry * 0.9f);
    effect.bypass = false;
    ASSERT_TRUE(replace());
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    engine.stopAll(0);
    settledPeak();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
}

TEST_F(AudioPlaybackRegression, EquivalentEffectSourcesKeepSeparatePlaybackGenerations)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    ASSERT_TRUE(VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(sounds[0]->objectID)},
         {QStringLiteral("property"), QStringLiteral("loopCount")},
         {QStringLiteral("value"), 0}}, VNX::ActionSource::GUI).success);
    auto* first = projects.createItem(nullptr, QStringLiteral("FirstLimiter"),
                                     VNX::ProjectItem::Type::Effect);
    auto* second = projects.createItem(nullptr, QStringLiteral("SecondLimiter"),
                                      VNX::ProjectItem::Type::Effect);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const auto setCeiling = [&](VNX::ProjectItem* preset, double ceiling) {
        return VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(preset->objectID)},
             {QStringLiteral("property"), QStringLiteral("ceilingDb")},
             {QStringLiteral("value"), ceiling}}, VNX::ActionSource::GUI).success;
    };
    ASSERT_TRUE(setCeiling(first, -24.0));
    ASSERT_TRUE(setCeiling(second, -24.0));
    VNX::EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.mode = VNX::EffectMode::Preset;
    effect.effectGuid = first->guid;
    const auto replace = [&] {
        return VNX::ActionManager::instance().execute(QStringLiteral("replace_field_group_effects"),
            {{QStringLiteral("definition_guid"), layer->guid.toString(QUuid::WithoutBraces)},
             {QStringLiteral("group"), QStringLiteral("effect")},
             {QStringLiteral("scope"), QStringLiteral("Shared")},
             {QStringLiteral("effects"), QJsonArray{effect.toJson()}}}, VNX::ActionSource::GUI).success;
    };
    ASSERT_TRUE(replace());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    auto* runtime = harken::detail::ActiveEngine();
    const auto installedGeneration = runtime->variables.CurrentWorldGeneration();
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    ASSERT_GT(pumpUntilPreviewAudible(), 0.0f);
    effect.effectGuid = second->guid;
    ASSERT_TRUE(replace());
    for (int block = 0; block < 64; ++block) {
        QCoreApplication::processEvents();
        engine.runtimeSyncCoordinator().flushPendingMutationNow();
        QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        runtime->nullBackend.Pump(1);
        QThread::msleep(1);
    }
    EXPECT_EQ(runtime->variables.CurrentWorldGeneration(), installedGeneration);
    const auto& coordinator = engine.runtimeSyncCoordinator();
    EXPECT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Pending);
    const float limited = settledPeak();
    ASSERT_GT(limited, 0.0f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> paths;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(paths));
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths.front().worldGeneration, installedGeneration);
    ASSERT_TRUE(setCeiling(second, 0.0));
    EXPECT_GT(settledPeak(), limited * 1.2f);
    engine.stopAll(0);
    settledPeak();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_NE(runtime->variables.CurrentWorldGeneration(), installedGeneration);
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    EXPECT_GT(settledPeak(), limited / 0.7f);
}

TEST_F(AudioPlaybackRegression, ScheduledVoiceAdoptsLatestLimiterSourceWhenItStarts)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    auto* preset = projects.createItem(nullptr, QStringLiteral("ScheduledReplacement"),
                                      VNX::ProjectItem::Type::Effect);
    auto* track = projects.createItem(layer, QStringLiteral("DelayedTrack"),
                                     VNX::ProjectItem::Type::Track);
    ASSERT_NE(preset, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_TRUE(projects.moveItem(sounds[0], track));
    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    ASSERT_EQ(timeline->clips.items.size(), 1u);
    auto& clip = timeline->clips.items.front();
    clip.start = 250000000;
    clip.duration = 1000000000;
    ASSERT_NE(projects.createActionTargeting(track, VNX::VActionStep::Type::Play,
                                             QStringLiteral("Play_Delayed")), nullptr);
    VNX::EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.mode = VNX::EffectMode::Custom;
    effect.custom.parameters = {-24.0f, 1.0f};
    const auto replace = [&] {
        return VNX::ActionManager::instance().execute(QStringLiteral("replace_field_group_effects"),
            {{QStringLiteral("definition_guid"), layer->guid.toString(QUuid::WithoutBraces)},
             {QStringLiteral("group"), QStringLiteral("effect")},
             {QStringLiteral("scope"), QStringLiteral("Shared")},
             {QStringLiteral("effects"), QJsonArray{effect.toJson()}}}, VNX::ActionSource::GUI).success;
    };
    ASSERT_TRUE(replace());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto generation = harken::detail::ActiveEngine()->variables.CurrentWorldGeneration();
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Delayed")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    EXPECT_EQ(settledPeak(), 0.0f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> paths;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(paths));
    ASSERT_TRUE(paths.empty());
    effect.mode = VNX::EffectMode::Preset;
    effect.effectGuid = preset->guid;
    ASSERT_TRUE(replace());
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    EXPECT_EQ(settledPeak(), 0.0f);
    EXPECT_EQ(harken::detail::ActiveEngine()->variables.CurrentWorldGeneration(), generation);
    ASSERT_GT(pumpUntilPreviewAudible(512), 0.0f);
    const float limited = settledPeak();
    ASSERT_GT(limited, 0.0f);
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(paths));
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths.front().worldGeneration, generation);
    effect.bypass = true;
    ASSERT_TRUE(replace());
    const float dry = settledPeak();
    EXPECT_GT(limited, dry * 0.9f);
    effect.bypass = false;
    ASSERT_TRUE(replace());
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
}

TEST_F(AudioPlaybackRegression, BusLimiterSourceReplacementAppliesDuringPlayback)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    auto* bus = projects.getMasterBusItem();
    ASSERT_NE(bus, nullptr);
    auto* preset = projects.createItem(nullptr, QStringLiteral("BusReplacementLimiter"),
                                      VNX::ProjectItem::Type::Effect);
    ASSERT_NE(preset, nullptr);
    ASSERT_TRUE(VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(preset->objectID)},
         {QStringLiteral("property"), QStringLiteral("releaseMs")},
         {QStringLiteral("value"), 1.0}}, VNX::ActionSource::GUI).success);
    VNX::EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.mode = VNX::EffectMode::Custom;
    effect.custom.parameters = {-24.0f, 1.0f};
    effect.bypass = true;
    VNX::EffectEntry added;
    added.guid = QUuid::createUuid();
    added.mode = VNX::EffectMode::Custom;
    bool includeAdded = false;
    const auto replace = [&] {
        QJsonArray effects{effect.toJson()};
        if (includeAdded) effects.append(added.toJson());
        return VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(bus->objectID)},
             {QStringLiteral("property"), QStringLiteral("effects")},
             {QStringLiteral("value"), effects}},
            VNX::ActionSource::GUI).success;
    };
    ASSERT_TRUE(replace());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    const float dry = settledPeak();
    ASSERT_GT(dry, 0.05f);
    effect.bypass = false;
    ASSERT_TRUE(replace());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const float limited = settledPeak();
    EXPECT_GT(limited, 0.0f);
    EXPECT_LT(limited, dry * 0.7f);

    effect.mode = VNX::EffectMode::Preset;
    effect.effectGuid = preset->guid;
    ASSERT_TRUE(replace());
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 100) {
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        QThread::msleep(1);
    }
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    effect.bypass = true;
    ASSERT_TRUE(replace());
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    effect.bypass = false;
    ASSERT_TRUE(replace());
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    engine.stopAll(0);
    settledPeak();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
              HarkenEditor::Bridge::kEngineOk);
    EXPECT_NEAR(settledPeak(), dry, 0.002f);

    ASSERT_TRUE(VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(preset->objectID)},
         {QStringLiteral("property"), QStringLiteral("ceilingDb")},
         {QStringLiteral("value"), -24.0}}, VNX::ActionSource::GUI).success);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_NEAR(settledPeak(), limited, 0.002f);
    includeAdded = true;
    ASSERT_TRUE(replace());
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    EXPECT_NEAR(settledPeak(), limited, 0.002f);
    ASSERT_TRUE(VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(preset->objectID)},
         {QStringLiteral("property"), QStringLiteral("ceilingDb")},
         {QStringLiteral("value"), 0.0}}, VNX::ActionSource::GUI).success);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
}

TEST_F(AudioPlaybackRegression, LiveEffectChainUsesInheritedPreviewAndRetriesLatestDesired) {
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult,
        HarkenEditor::Bridge::kEngineOk);
    const float dry = settledPeak();
    ASSERT_GT(dry, 0.05f);
    auto* runtime = harken::detail::ActiveEngine();
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> paths;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(paths));
    ASSERT_EQ(1u, paths.size());
    const auto handle = paths[0].voiceInstance;
    auto* render = runtime->voices.Resolve(handle);
    ASSERT_NE(nullptr, render);
    VNX::EffectEntry entry;
    entry.guid = QUuid::createUuid();
    entry.mode = VNX::EffectMode::Custom;
    entry.custom.parameters = {-24.0f, 20.0f};
    auto parent = layer->fieldGroups;
    auto child = sounds[0]->fieldGroups;
    child.effects.shared.mode = VNX::FieldGroupMode::Inherit;
    projects.fieldGroupsPreviewed(sounds[0], child);
    const auto previewParent = [&](const std::vector<VNX::EffectEntry>& entries) {
        VNX::EffectFieldGroup group;
        EXPECT_TRUE(group.effects.replace(entries));
        parent.effects.shared = {VNX::FieldGroupMode::Local, group};
        projects.fieldGroupsPreviewed(layer, parent);
    };
    previewParent({entry});
    EXPECT_NEAR(settledPeak(), dry * std::pow(10.0f, -24.0f / 20.0f) / 0.125f, 0.002f);
    const auto ref = render->fxSlotEffectRef[0];
    entry.custom.parameters[0] = -21;
    previewParent({entry});
    EXPECT_NEAR(settledPeak(), dry * std::pow(10.0f, -21.0f / 20.0f) / 0.125f, 0.002f);
    EXPECT_EQ(ref.poolIndex, render->fxSlotEffectRef[0].poolIndex);
    EXPECT_EQ(ref.generation, render->fxSlotEffectRef[0].generation);
    entry.guid = QUuid::createUuid();
    entry.custom.parameters[0] = -18;
    previewParent({entry});
    QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
    entry.guid = QUuid::createUuid();
    entry.custom.parameters[0] = -23;
    previewParent({entry});
    QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
    EXPECT_NEAR(settledPeak(), dry * std::pow(10.0f, -23.0f / 20.0f) / 0.125f, 0.002f);
    EXPECT_EQ(entry.runtimeId().high, runtime->effectBank.ObjectIdentity(render->fxSlotEffectRef[0]).effectId.high);
    EXPECT_EQ(entry.runtimeId().low, runtime->effectBank.ObjectIdentity(render->fxSlotEffectRef[0]).effectId.low);
    entry.bypass = true;
    previewParent({entry});
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    child.effects.shared = {VNX::FieldGroupMode::Local, VNX::EffectFieldGroup{}};
    projects.fieldGroupsPreviewed(sounds[0], child);
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    EXPECT_EQ(harken::kEffectTypeNone, render->fxSlotEffectRef[0].type);
    entry.bypass = false;
    previewParent({entry});
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    child.effects.shared.mode = VNX::FieldGroupMode::Inherit;
    projects.fieldGroupsPreviewed(sounds[0], child);
    const float limited = settledPeak();
    EXPECT_NEAR(limited, dry * std::pow(10.0f, -23.0f / 20.0f) / 0.125f, 0.002f);
    VNX::EffectEntry unresolved;
    unresolved.guid = QUuid::createUuid();
    unresolved.effectGuid = QUuid::createUuid();
    entry.custom.parameters[0] = 0.0f;
    previewParent({entry, unresolved});
    // An unresolved reference rejects the snapshot as a whole, preserving accepted audio.
    EXPECT_NEAR(settledPeak(), limited, 0.002f);
    entry.custom.parameters[0] = -23.0f;
    previewParent({entry});
    EXPECT_NEAR(settledPeak(), limited, 0.002f);
    const auto accepted = render->fxSlotEffectRef[0];
    previewParent({unresolved});
    EXPECT_NEAR(settledPeak(), limited, 0.002f);
    EXPECT_EQ(accepted.poolIndex, render->fxSlotEffectRef[0].poolIndex);
    EXPECT_EQ(accepted.generation, render->fxSlotEffectRef[0].generation);
    previewParent({});
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    EXPECT_EQ(harken::kEffectTypeNone, render->fxSlotEffectRef[0].type);
}

TEST_F(AudioPlaybackRegression, LiveEffectPresetActionsReachEveryInheritedReference) {
    VNX::registerProjectActions(VNX::ActionManager::instance(), &projects, nullptr);
    auto* preset = projects.createItem(nullptr, QStringLiteral("SharedLimiter"), VNX::ProjectItem::Type::Effect);
    ASSERT_NE(nullptr, preset);
    auto* secondAction = projects.createActionTargeting(sounds[1], VNX::VActionStep::Type::Play,
        QStringLiteral("Play_Second"));
    ASSERT_NE(nullptr, secondAction);
    const auto mode = [&](VNX::ProjectItem* item, const char* value) {
        return VNX::ActionManager::instance().execute(QStringLiteral("set_field_group_mode"),
            {{QStringLiteral("definition_guid"), item->guid.toString(QUuid::WithoutBraces)},
             {QStringLiteral("group"), QStringLiteral("effect")},
             {QStringLiteral("scope"), QStringLiteral("Shared")},
             {QStringLiteral("mode"), QString::fromLatin1(value)}}, VNX::ActionSource::IPC);
    };
    for (auto* sound : sounds) {
        const auto childMode = mode(sound, "Inherit");
        ASSERT_TRUE(childMode.success) << childMode.errorMessage.toStdString();
    }
    VNX::EffectEntry first;
    first.guid = QUuid::createUuid();
    first.effectGuid = preset->guid;
    auto second = first;
    second.guid = QUuid::createUuid();
    const auto replaced = VNX::ActionManager::instance().execute(QStringLiteral("replace_field_group_effects"),
        {{QStringLiteral("definition_guid"), layer->guid.toString(QUuid::WithoutBraces)},
         {QStringLiteral("group"), QStringLiteral("effect")},
         {QStringLiteral("scope"), QStringLiteral("Shared")},
         {QStringLiteral("effects"), QJsonArray{first.toJson(), second.toJson()}}}, VNX::ActionSource::IPC);
    ASSERT_TRUE(replaced.success) << replaced.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Good")).engineResult, HarkenEditor::Bridge::kEngineOk);
    ASSERT_EQ(engine.playByNameDetailed(QStringLiteral("Play_Second")).engineResult, HarkenEditor::Bridge::kEngineOk);
    const float dry = settledPeak();
    ASSERT_GT(dry, 0.1f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> paths;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(paths));
    ASSERT_EQ(2u, paths.size());
    auto* runtime = harken::detail::ActiveEngine();
    std::array<std::array<harken::EffectInstanceRef, 8>, 2> before;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto* voice = runtime->voices.Resolve(paths[i].voiceInstance);
        ASSERT_NE(nullptr, voice);
        std::copy_n(voice->fxSlotEffectRef, 8, before[i].begin());
    }
    EXPECT_NE(before[0][0].poolIndex, before[0][1].poolIndex);
    EXPECT_NE(before[1][0].poolIndex, before[1][1].poolIndex);
    for (std::uint32_t a = 0; a < 2; ++a)
        for (std::uint32_t b = 0; b < 2; ++b)
            EXPECT_NE(before[0][a].poolIndex, before[1][b].poolIndex);
    const auto setPreset = [&](const QString& property, float value) {
        return VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(preset->objectID)},
             {QStringLiteral("property"), property}, {QStringLiteral("value"), value}}, VNX::ActionSource::IPC);
    };
    ASSERT_TRUE(setPreset(QStringLiteral("ceilingDb"), -24).success);
    ASSERT_TRUE(setPreset(QStringLiteral("releaseMs"), 17).success);
    EXPECT_NEAR(settledPeak(), dry * (2.0f * std::pow(10.0f, -24.0f / 20.0f) / 0.375f), 0.002f);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto handle = paths[i].voiceInstance;
        const auto* voice = runtime->voices.Resolve(handle);
        ASSERT_NE(nullptr, voice);
        for (std::uint32_t slot = 0; slot < 2; ++slot) {
            EXPECT_EQ(before[i][slot].poolIndex, voice->fxSlotEffectRef[slot].poolIndex);
            EXPECT_EQ(before[i][slot].generation, voice->fxSlotEffectRef[slot].generation);
            EXPECT_FLOAT_EQ(-24, runtime->effectSlotConfig[static_cast<std::uint32_t>(handle)][slot].params[0]);
            EXPECT_FLOAT_EQ(17, runtime->effectSlotConfig[static_cast<std::uint32_t>(handle)][slot].params[1]);
        }
    }
    first.bypass = second.bypass = true;
    const auto bypassed = VNX::ActionManager::instance().execute(QStringLiteral("replace_field_group_effects"),
        {{QStringLiteral("definition_guid"), layer->guid.toString(QUuid::WithoutBraces)},
         {QStringLiteral("group"), QStringLiteral("effect")},
         {QStringLiteral("scope"), QStringLiteral("Shared")},
         {QStringLiteral("effects"), QJsonArray{second.toJson(), first.toJson()}}}, VNX::ActionSource::IPC);
    ASSERT_TRUE(bypassed.success) << bypassed.errorMessage.toStdString();
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    EXPECT_NEAR(settledPeak(), dry, 0.002f);
    for (const auto operation : {VNX::RuntimeCapabilityOperation::CompiledPreview,
                                  VNX::RuntimeCapabilityOperation::PackBuild}) {
        const auto capability = VNX::RuntimeCapabilityMatrix::evaluate(*sounds[0],
            *projects.currentProject(), operation, VNX::RuntimeCapabilityFeature::TypedEffect);
        EXPECT_EQ(VNX::RuntimeCapabilityState::Ready, capability.state)
            << capability.diagnostic.toStdString();
    }
    for (const auto& path : paths) {
        const auto* voice = runtime->voices.Resolve(path.voiceInstance);
        ASSERT_NE(nullptr, voice);
        EXPECT_EQ(second.runtimeId().high, runtime->effectBank.ObjectIdentity(voice->fxSlotEffectRef[0]).effectId.high);
        EXPECT_EQ(second.runtimeId().low, runtime->effectBank.ObjectIdentity(voice->fxSlotEffectRef[0]).effectId.low);
        EXPECT_EQ(first.runtimeId().high, runtime->effectBank.ObjectIdentity(voice->fxSlotEffectRef[1]).effectId.high);
        EXPECT_EQ(first.runtimeId().low, runtime->effectBank.ObjectIdentity(voice->fxSlotEffectRef[1]).effectId.low);
        EXPECT_FLOAT_EQ(1.0f, voice->fxBypassMix[0]);
        EXPECT_FLOAT_EQ(1.0f, voice->fxBypassMix[1]);
    }
}

TEST_F(AudioPlaybackRegression, ScalarEditCannotPublishCachedWorldAfterInvalidStructuralEdit)
{
    auto& coordinator = engine.runtimeSyncCoordinator();
    const auto generation = harken::detail::ActiveEngine()->variables.CurrentWorldGeneration();
    action->comp<VNX::VAction>()->steps.front().type =
        static_cast<VNX::VActionStep::Type>(255);
    coordinator.markCurrentWorldMutation();
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*sounds[0]);
    ASSERT_TRUE(locator);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *sounds[0], *locator, 1, -6.0, std::nullopt));
    coordinator.markCurrentWorldPropertyMutation(sounds[0]->runtimeId(), 1);
    coordinator.flushPendingMutationNow();
    EXPECT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);
    EXPECT_EQ(harken::detail::ActiveEngine()->variables.CurrentWorldGeneration(), generation);
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

TEST_F(AudioPlaybackRegression, ActionCatalogGrowthPreservesPlaybackAndSession)
{
    namespace Bridge = HarkenEditor::Bridge;
    auto& coordinator = engine.runtimeSyncCoordinator();
    const auto epoch = coordinator.barrier(coordinator.currentAuthoringRevision()).runtimeEpoch;
    for (int index = 1; index < 256; ++index) {
        ASSERT_NE(projects.createActionTargeting(
            sounds[0], VNX::VActionStep::Type::Play,
            QStringLiteral("CatalogAction%1").arg(index)), nullptr);
    }
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto started = engine.playByNameDetailed(QStringLiteral("CatalogAction255"));
    ASSERT_EQ(started.engineResult, Bridge::kEngineOk);
    const auto generation = harken::detail::ActiveEngine()->variables.CurrentWorldGeneration();
    ASSERT_GT(settledPeak(), 0.05f);
    ASSERT_NE(projects.createActionTargeting(
        sounds[0], VNX::VActionStep::Type::Play,
        QStringLiteral("CatalogAction256")), nullptr);
    coordinator.flushPendingMutationNow();
    ASSERT_GT(settledPeak(), 0.05f);
    EXPECT_TRUE(Bridge::Detail::runtimeOwnsPlayback(started.playbackHandle));
    EXPECT_EQ(harken::detail::ActiveEngine()->variables.CurrentWorldGeneration(), generation);
    engine.stopPlayback(started.playbackHandle);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).runtimeEpoch, epoch);
    const auto grown = engine.playByNameDetailed(QStringLiteral("CatalogAction256"));
    ASSERT_EQ(grown.engineResult, Bridge::kEngineOk);
    ASSERT_GT(settledPeak(), 0.05f);
    engine.stopPlayback(grown.playbackHandle);
    settledPeak();
    engine.stopOutput();
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(engine.playByNameDetailed(QStringLiteral("CatalogAction256")).engineResult,
              Bridge::kEngineOk);
    EXPECT_GT(settledPeak(), 0.05f);
}

TEST_F(AudioPlaybackRegression, InvalidAuthoringPreservesInstalledSoundState)
{
    auto* runtime = harken::detail::ActiveEngine();
    const auto revision = runtime->registry.Lookup(harkentest::ToRuntime(sounds[0]->runtimeId())).contentRevision;
    const auto generation = runtime->variables.CurrentWorldGeneration();
    writeWavF32(paths[0], std::vector<float>(65536, 0.5f));
    ASSERT_TRUE(QFile::remove(paths[1]));
    action->comp<VNX::VAction>()->steps.front().type =
        static_cast<VNX::VActionStep::Type>(255);
    auto& coordinator = engine.runtimeSyncCoordinator();
    coordinator.markCurrentWorldMutation();
    coordinator.flushPendingMutationNow();
    const auto barrier = coordinator.barrier(coordinator.currentAuthoringRevision());
    EXPECT_EQ(barrier.state, VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);
    EXPECT_EQ(runtime->variables.CurrentWorldGeneration(), generation);
    EXPECT_EQ(runtime->registry.Lookup(harkentest::ToRuntime(sounds[0]->runtimeId())).contentRevision, revision);
    EXPECT_TRUE(HarkenEditor::Bridge::runtimeObjectExists(sounds[1]->runtimeId()));
}

TEST_F(AudioPlaybackRegression, CompilerRefusalPreservesPreparedSoundState)
{
    namespace Bridge = HarkenEditor::Bridge;
    auto* runtime = harken::detail::ActiveEngine();
    const auto revision = runtime->registry.Lookup(harkentest::ToRuntime(sounds[0]->runtimeId())).contentRevision;
    const auto generation = runtime->variables.CurrentWorldGeneration();
    writeWavF32(paths[0], std::vector<float>(65536, 0.5f));
    ASSERT_TRUE(QFile::remove(paths[1]));
    auto preparation = std::make_shared<Bridge::CurrentWorldSoundPreparation>();
    QString error;
    ASSERT_TRUE(VNX::ProjectManager::prepareRuntimeSounds(
        *projects.currentProject(), *preparation, &error));
    VNX::CurrentWorldBuildOptions options;
    options.isolateUnavailable = true;
    options.soundContentRevisions = &preparation->contentRevisions;
    auto request = VNX::buildCurrentWorldSyncRequest(
        *projects.currentProject(), std::nullopt, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    request->soundPreparation = std::move(preparation);
    ASSERT_FALSE(request->actions.empty());
    ASSERT_FALSE(request->actions.front().steps.empty());
    request->actions.front().steps.front().opcode = static_cast<harkenstudiobuild::ActionOpcodeV1>(255);
    const auto rejected = Bridge::syncCurrentWorld(std::move(request), {71, 72, 73, 74});
    EXPECT_FALSE(rejected.ok());
    EXPECT_FALSE(rejected.pending());
    EXPECT_EQ(runtime->variables.CurrentWorldGeneration(), generation);
    EXPECT_EQ(runtime->registry.Lookup(harkentest::ToRuntime(sounds[0]->runtimeId())).contentRevision, revision);
    EXPECT_TRUE(Bridge::runtimeObjectExists(sounds[1]->runtimeId()));
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, SourceRefreshPreservesPlaybackAcrossFileAndRegistrationChanges)
{
    namespace Bridge = HarkenEditor::Bridge;
    auto& media = VNX::MediaPool::instance();
    auto& registry = VNX::AssetRegistry::instance();
    const auto waitForMedia = qScopeGuard([] { QThreadPool::globalInstance()->waitForDone(); });
    constexpr uint32_t sourceId = 930001;
    const auto soundId = sounds[0]->runtimeId();
    auto* runtime = harken::detail::ActiveEngine();
    const auto originalRevision = runtime->registry.Lookup(harkentest::ToRuntime(soundId)).contentRevision;
    ASSERT_NE(originalRevision, 0u);

    ASSERT_TRUE(media.refreshSourceFile(sourceId, paths[0]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(runtime->registry.Lookup(harkentest::ToRuntime(soundId)).contentRevision, originalRevision);
    expectGoodAction();

    QThreadPool::globalInstance()->waitForDone();
    const auto modified = QFileInfo(paths[0]).lastModified();
    writeWavF32(paths[0], std::vector<float>(65536, 0.5f));
    QFile replacement(paths[0]);
    ASSERT_TRUE(replacement.open(QIODevice::ReadWrite));
    ASSERT_TRUE(replacement.setFileTime(modified, QFileDevice::FileModificationTime));
    replacement.close();
    ASSERT_TRUE(media.refreshSourceFile(sourceId, paths[0]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto replacedRevision = runtime->registry.Lookup(harkentest::ToRuntime(soundId)).contentRevision;
    EXPECT_NE(replacedRevision, originalRevision);
    EXPECT_NE(replacedRevision, 0u);

    QThreadPool::globalInstance()->waitForDone();
    const QString relocated = QFileInfo(paths[0]).dir().filePath(QStringLiteral("Relocated.wav"));
    ASSERT_TRUE(QFile::rename(paths[0], relocated));
    registry.relocateSourcePaths(QFileInfo(paths[0]).fileName(), QFileInfo(relocated).fileName());
    paths[0] = relocated;
    ASSERT_TRUE(media.refreshSourceFile(sourceId, paths[0]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(Bridge::mediaRegistryFindPath(sourceId), paths[0]);
    EXPECT_EQ(runtime->registry.Lookup(harkentest::ToRuntime(soundId)).contentRevision, replacedRevision);
    expectGoodAction();

    QThreadPool::globalInstance()->waitForDone();
    ASSERT_TRUE(QFile::remove(paths[0]));
    VNX::ProjectManager::resyncSourceToRuntime(sourceId);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_FALSE(Bridge::runtimeObjectExists(soundId));
    EXPECT_TRUE(Bridge::runtimeObjectExists(sounds[1]->runtimeId()));

    writeWavF32(paths[0], std::vector<float>(65536, 0.5f));
    ASSERT_TRUE(media.refreshSourceFile(sourceId, paths[0]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_TRUE(Bridge::runtimeObjectExists(soundId));
    EXPECT_EQ(runtime->registry.Lookup(harkentest::ToRuntime(soundId)).contentRevision, replacedRevision);
    expectGoodAction();

    Bridge::removeRuntimeObject(soundId);
    ASSERT_FALSE(Bridge::runtimeObjectExists(soundId));
    ASSERT_TRUE(media.refreshSourceFile(sourceId, paths[0]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_TRUE(Bridge::runtimeObjectExists(soundId));
    expectGoodAction();

    engine.clearRuntimeObjects();
    VNX::ProjectManager::syncActionNameForPreview(action);
    Bridge::mediaRegistryUnregister(sourceId);
    ASSERT_EQ(Bridge::registeredSoundRevision(soundId), 0u);
    ASSERT_TRUE(media.refreshSourceFile(sourceId, paths[0]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(Bridge::mediaRegistryFindPath(sourceId), paths[0]);
    EXPECT_EQ(Bridge::registeredSoundRevision(soundId), replacedRevision);
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

TEST_F(AudioPlaybackRegression, AssetNotificationsReloadSourceContentWithoutRefreshedFingerprint)
{
    auto& registry = VNX::AssetRegistry::instance();
    constexpr uint32_t sourceId = 930001;
    const auto soundId = sounds[0]->runtimeId();
    auto* runtime = harken::detail::ActiveEngine();
    const auto originalRevision = runtime->registry.Lookup(harkentest::ToRuntime(soundId)).contentRevision;
    const auto originalHash = registry.getAsset(sourceId)->contentHash;
    const QFileInfo originalFile(paths[0]);
    const auto modified = originalFile.lastModified();
    const auto fileSize = originalFile.size();
    writeWavF32(paths[0], std::vector<float>(65536, 0.5f));
    QFile replacement(paths[0]);
    ASSERT_TRUE(replacement.open(QIODevice::ReadWrite));
    ASSERT_TRUE(replacement.setFileTime(modified, QFileDevice::FileModificationTime));
    replacement.close();
    ASSERT_EQ(QFileInfo(paths[0]).size(), fileSize);
    ASSERT_EQ(QFileInfo(paths[0]).lastModified(), modified);
    ASSERT_EQ(registry.getAsset(sourceId)->contentHash, originalHash);
    ASSERT_NE(VNX::AssetImporter::computeContentHash(paths[0]), originalHash);

    emit registry.assetModified(sourceId);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_NE(runtime->registry.Lookup(harkentest::ToRuntime(soundId)).contentRevision, originalRevision);
    EXPECT_TRUE(HarkenEditor::Bridge::runtimeObjectExists(soundId));
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

    bool waitForActionVoices(
        std::size_t expectedCount, std::vector<VnVoiceHandle>& voices)
    {
        auto* runtime = harken::detail::ActiveEngine();
        if (runtime == nullptr)
            return false;
        for (int attempt = 0; attempt < 128; ++attempt) {
            if (Vn_Update() != VN_OK)
                return false;
            runtime->nullBackend.Pump(1);
            if (Vn_Update() != VN_OK)
                return false;
            voices.clear();
            for (const auto& row : runtime->playableServices) {
                if (row.phase == harken::EnginePlayableStartPhase::Started
                    && row.InvocationValid()
                    && row.Invocation().actionFiring != 0
                    && row.renderHandle != 0) {
                    voices.push_back(row.renderHandle);
                }
            }
            if (voices.size() >= expectedCount)
                return true;
            QThread::yieldCurrentThread();
        }
        return false;
    }

    bool waitForSeekTerminal(
        uint64_t operationId, VNX::ActionResult& terminal)
    {
        for (int attempt = 0; attempt < 128; ++attempt) {
            if (!QMetaObject::invokeMethod(
                    &engine, "onRuntimeServiceTimer", Qt::DirectConnection)) {
                return false;
            }
            terminal = query(operationId);
            if (!terminal.success
                || terminal.data.value(QStringLiteral("state")).toString()
                    != QStringLiteral("accepted")) {
                return true;
            }
            QThread::yieldCurrentThread();
        }
        return false;
    }

    quintptr postAction()
    {
        const auto result = engine.playByNameDetailed(action->name);
        EXPECT_EQ(result.engineResult, HarkenEditor::Bridge::kEngineOk);
        EXPECT_NE(result.playbackHandle, 0u);
        return result.playbackHandle;
    }

    VNX::ActionResult seek(quintptr handle, double positionMs,
                           bool relative = false,
                           const QString& expectedName = {})
    {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("seek_action"),
            QJsonObject{
                {QStringLiteral("action_handle"),
                 QString::number(static_cast<qulonglong>(handle))},
                {QStringLiteral("name"),
                 expectedName.isEmpty() ? action->name : expectedName},
                {QStringLiteral("position"), positionMs},
                {QStringLiteral("is_relative"), relative},
                {QStringLiteral("snap_to_marker"), false},
            },
            VNX::ActionSource::IPC);
    }

    VNX::ActionResult query(uint64_t operationId)
    {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("query_seek_action"),
            QJsonObject{{QStringLiteral("seek_operation_id"),
                         QString::number(operationId)}},
            VNX::ActionSource::IPC);
    }

    VNX::ActionResult release(uint64_t operationId)
    {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("release_seek_action"),
            QJsonObject{{QStringLiteral("seek_operation_id"),
                         QString::number(operationId)}},
            VNX::ActionSource::IPC);
    }

    static uint64_t operationId(const VNX::ActionResult& result)
    {
        const QJsonValue value =
            result.data.value(QStringLiteral("seek_operation_id"));
        if (value.isString())
            return value.toString().toULongLong();
        return static_cast<uint64_t>(value.toInteger());
    }

    VNX::AudioEngine& engine = VNX::AudioEngine::instance();
    VNX::ProjectManager& projects = VNX::ProjectManager::instance();
    QTemporaryDir directory;
    QString wavPath;
    VNX::ProjectItem* sound = nullptr;
    VNX::ProjectItem* action = nullptr;
};

TEST_F(StudioActionSeekTest,
       ExactHandleIsAsynchronousAndTargetsOnlyOneConcurrentFiring)
{
    const quintptr first = postAction();
    const quintptr second = postAction();
    ASSERT_NE(first, second);

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    std::vector<VnVoiceHandle> actionVoices;
    ASSERT_TRUE(waitForActionVoices(2, actionVoices));
    ASSERT_EQ(actionVoices.size(), 2u);

    const double frameMs = 1000.0 / 48000.0;
    const auto started = seek(first, 1024.0 * frameMs);
    ASSERT_TRUE(started.success) << started.errorMessage.toStdString();
    const uint64_t operation = operationId(started);
    ASSERT_NE(operation, 0u);
    EXPECT_EQ(started.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("accepted"));

    const auto accepted = query(operation);
    ASSERT_TRUE(accepted.success) << accepted.errorMessage.toStdString();
    EXPECT_EQ(accepted.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("accepted"));

    VNX::ActionResult applied;
    ASSERT_TRUE(waitForSeekTerminal(operation, applied));
    ASSERT_TRUE(applied.success) << applied.errorMessage.toStdString();
    EXPECT_EQ(applied.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("applied"));
    int selectedCount = 0;
    int untouchedCount = 0;
    for (const VnVoiceHandle voice : actionVoices) {
        const auto* render = runtime->voices.Resolve(voice);
        ASSERT_NE(render, nullptr);
        selectedCount += render->sourceActivationStartFrame == 1024 ? 1 : 0;
        untouchedCount += render->sourceActivationStartFrame == 0 ? 1 : 0;
    }
    EXPECT_EQ(selectedCount, 1);
    EXPECT_EQ(untouchedCount, 1);

    ASSERT_TRUE(release(operation).success);
    const auto staleOperation = query(operation);
    EXPECT_FALSE(staleOperation.success);
}

TEST_F(StudioActionSeekTest,
       RestartInvalidatesOldIdsWithoutReusingPlaybackOrOperationHandles)
{
    const quintptr oldPlayback = postAction();
    std::vector<VnVoiceHandle> voices;
    ASSERT_TRUE(waitForActionVoices(1, voices));
    const auto oldSeek = seek(oldPlayback, 10.0);
    ASSERT_TRUE(oldSeek.success) << oldSeek.errorMessage.toStdString();
    const uint64_t oldOperation = operationId(oldSeek);
    VNX::ActionResult oldTerminal;
    ASSERT_TRUE(waitForSeekTerminal(oldOperation, oldTerminal));
    ASSERT_TRUE(oldTerminal.success);

    engine.shutdown();
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 16;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    const auto stalePlayback = seek(oldPlayback, 10.0);
    EXPECT_FALSE(stalePlayback.success);
    const auto staleOperation = query(oldOperation);
    EXPECT_FALSE(staleOperation.success);

    const quintptr nextPlayback = postAction();
    ASSERT_NE(nextPlayback, oldPlayback);
    voices.clear();
    ASSERT_TRUE(waitForActionVoices(1, voices));
    const auto nextSeek = seek(nextPlayback, 10.0);
    ASSERT_TRUE(nextSeek.success) << nextSeek.errorMessage.toStdString();
    const uint64_t nextOperation = operationId(nextSeek);
    EXPECT_NE(nextOperation, oldOperation);
    VNX::ActionResult nextTerminal;
    ASSERT_TRUE(waitForSeekTerminal(nextOperation, nextTerminal));
    EXPECT_TRUE(nextTerminal.success);
    EXPECT_TRUE(release(nextOperation).success);
}

TEST_F(StudioActionSeekTest, EngineShutdownCompletesPendingAuditionSeekAsFailure)
{
    HarkenEditor::AuditionManager audition(&engine);
    const quintptr playback = postAction();
    std::vector<VnVoiceHandle> voices;
    ASSERT_TRUE(waitForActionVoices(1, voices));
    const uint32_t playingId = audition.registerExternalPlayback(
        QStringLiteral("SeekAction"), playback);
    ASSERT_NE(playingId, 0u);

    int finishedSignals = 0;
    bool applied = true;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionSeekFinished,
        &audition, [&](uint32_t id, quint64, int32_t,
                       bool didApply) {
            if (id == playingId) {
                ++finishedSignals;
                applied = didApply;
            }
        });
    const auto started = audition.setPositionSamples(playingId, 1024);
    ASSERT_EQ(started.result, HarkenEditor::Bridge::kEngineOk);

    const auto pending = audition.seekStatus(started.requestId);
    ASSERT_TRUE(pending.has_value());
    EXPECT_TRUE(pending->inProgress);
    engine.shutdown();
    EXPECT_EQ(finishedSignals, 1);
    EXPECT_FALSE(applied);
    const auto completed = audition.seekStatus(started.requestId);
    ASSERT_TRUE(completed.has_value());
    EXPECT_FALSE(completed->inProgress);
    EXPECT_FALSE(completed->applied);
    EXPECT_EQ(completed->result, HarkenEditor::Bridge::kEngineNotInit);
    EXPECT_EQ(completed->samplePosition, 1024);
}

TEST_F(StudioActionSeekTest, NativeQueryFailureCompletesPendingAuditionSeek)
{
    HarkenEditor::AuditionManager audition(&engine);
    const quintptr playback = postAction();
    std::vector<VnVoiceHandle> voices;
    ASSERT_TRUE(waitForActionVoices(1, voices));
    const uint32_t playingId = audition.registerExternalPlayback(
        QStringLiteral("SeekAction"), playback);
    ASSERT_NE(playingId, 0u);

    int finishedSignals = 0;
    quint64 finishedRequestId = 0;
    bool applied = true;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionSeekFinished,
        &audition, [&](uint32_t id, quint64 requestId, int32_t,
                       bool didApply) {
            if (id == playingId) {
                ++finishedSignals;
                finishedRequestId = requestId;
                applied = didApply;
                const auto status = audition.seekStatus(requestId);
                ASSERT_TRUE(status.has_value());
                EXPECT_FALSE(status->inProgress);
                EXPECT_EQ(status->applied, didApply);
            }
        });

    const auto started = audition.setPositionSamples(playingId, 1024);
    ASSERT_EQ(started.result, HarkenEditor::Bridge::kEngineOk);
    ASSERT_NE(started.requestId, 0u);

    ASSERT_EQ(Vn_Term(), VN_OK);
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &audition, "pollAuditionStates", Qt::DirectConnection));

    EXPECT_EQ(finishedSignals, 1);
    EXPECT_EQ(finishedRequestId, started.requestId);
    EXPECT_FALSE(applied);
    const auto completed = audition.seekStatus(started.requestId);
    ASSERT_TRUE(completed.has_value());
    EXPECT_FALSE(completed->inProgress);
    EXPECT_FALSE(completed->applied);
    EXPECT_NE(completed->result, HarkenEditor::Bridge::kEngineOk);

    ASSERT_TRUE(QMetaObject::invokeMethod(
        &audition, "pollAuditionStates", Qt::DirectConnection));
    EXPECT_EQ(finishedSignals, 1);
}

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
