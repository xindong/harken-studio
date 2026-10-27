#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Engine.h"
#include "AudioEngine.h"
#include "CompileTargetProfileCatalog.h"
#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/MediaPool.h"
#include "CurrentWorldProjectAdapter.h"
#include "Components/VAction.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "PropertyEditSession.h"
#include "UndoCommands.h"
#include "RuntimeSyncCoordinator.h"
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
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
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

    engine.shutdown();
    EXPECT_EQ(finishedSignals, 1);
    EXPECT_FALSE(applied);
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

    ASSERT_TRUE(QMetaObject::invokeMethod(
        &audition, "pollAuditionStates", Qt::DirectConnection));
    EXPECT_EQ(finishedSignals, 1);
}

}
