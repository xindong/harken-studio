#include "UnitTest/StudioObjectIdTestSupport.h"
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
    static char arg0[] = "harken_studio_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

std::shared_ptr<HarkenEditor::Build::CurrentWorldSyncRequest>
makeValidCurrentWorldRequest()
{
    auto request =
        std::make_shared<HarkenEditor::Build::CurrentWorldSyncRequest>();
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
    ASSERT_TRUE(HarkenEditor::Build::syncCurrentWorld(
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
        if (VnPreview_EnableChannelPeakMetering() != VN_OK)
            return 0.0f;
        takeMasterPeak();
        float peak = 0.0f;
        for (uint32_t block = 0; block < blocks; ++block) {
            engine.serviceRuntime();
            VnPreview_PumpNullMixer(1);
            peak = std::max(peak, takeMasterPeak());
            engine.serviceRuntime();
            poll();
        }
        return peak;
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

TEST_F(AuditionLifecycleTest, QueuedStartsUsePublishedDefinitionsAcrossPlaybackKinds)
{
    makeLooping();
    auto& coordinator = engine.runtimeSyncCoordinator();
    VNX::ProjectItem* added = nullptr;
    int started = 0;
    QObject::connect(&audition, &HarkenEditor::AuditionManager::auditionStarted,
                     &audition, [&](uint32_t, const QString&) { ++started; });
    {
        coordinator.beginSubmissionBatch();
        const auto release = qScopeGuard([&] { coordinator.endSubmissionBatch(); });
        added = projects.createAudioItem(layer, QStringLiteral("AddedSound"), paths[1], 930002);
        ASSERT_NE(added, nullptr);
        added->comp<VNX::VAudio>()->loopCount = 0;
        auto& step = action->comp<VNX::VAction>()->steps.front();
        step.targetObjectID = sounds[1]->objectID;
        step.targetName = sounds[1]->name;
        coordinator.markCurrentWorldMutation();

        ASSERT_NE(audition.playItem(*added), 0u);
        ASSERT_NE(audition.playItem(*action), 0u);
        EXPECT_EQ(started, 0);
        EXPECT_FLOAT_EQ(pump(32), 0.0f);
        std::vector<HarkenEditor::Bridge::PlaybackPathObservation> playback;
        ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(playback));
        EXPECT_TRUE(playback.empty());
    }
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    QElapsedTimer timeout;
    timeout.start();
    while (started < 2 && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        pump(1);
        QThread::msleep(1);
    }
    ASSERT_EQ(started, 2);
    EXPECT_GT(pump(32), 0.05f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> playback;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(playback));
    ASSERT_EQ(playback.size(), 2u);
    EXPECT_EQ(std::count_if(playback.begin(), playback.end(), [&](const auto& path) {
        return path.soundId == added->runtimeId();
    }), 1);
    EXPECT_EQ(std::count_if(playback.begin(), playback.end(), [&](const auto& path) {
        return path.soundId == sounds[1]->runtimeId();
    }), 1);
}

TEST_F(AuditionLifecycleTest, PendingAuthoringBlocksNewOperationsWithoutBlockingExistingAudio)
{
    makeLooping();
    auto* container = projects.createParentContainer(
        {sounds[1]}, VNX::ProjectItem::Type::Container, QStringLiteral("MemoryContainer"));
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto running = audition.playItem(*action);
    ASSERT_NE(running, 0u);
    EXPECT_GT(pump(32), 0.05f);

    auto& coordinator = engine.runtimeSyncCoordinator();
    coordinator.beginSubmissionBatch();
    const auto release = qScopeGuard([&] { coordinator.endSubmissionBatch(); });
    coordinator.markCurrentWorldMutation();
    for (const auto& result : {
             engine.playByNameDetailed(action->name),
             engine.playByNameWithPreviewContext(action->name),
             engine.playByName3DDetailed(action->name, {})}) {
        EXPECT_NE(result.engineResult, HarkenEditor::Bridge::kEngineOk);
        EXPECT_EQ(result.playbackHandle, 0u);
    }
    for (const auto& result : {
             engine.playPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableSound,
                                        sounds[0]->runtimeId()),
             engine.playPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableContainer,
                                        container->runtimeId())}) {
        EXPECT_NE(result.status, HarkenEditor::Bridge::kPreviewOk);
        EXPECT_EQ(result.handle, 0u);
    }
    EXPECT_NE(engine.resetPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableContainer,
                                          container->runtimeId()),
              HarkenEditor::Bridge::kPreviewOk);
    EXPECT_GT(pump(32), 0.05f);
    audition.stop(running);
    pump(32);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
    EXPECT_FLOAT_EQ(pump(32), 0.0f);

    const auto rawSound = engine.mintRawPcmSoundId();
    ASSERT_EQ(engine.registerRawPcmSlice(rawSound, std::vector<float>(4096, 0.25f),
                                        1, 48000, 0, 4096, true).result,
              HarkenEditor::Bridge::kEngineOk);
    const auto raw = engine.playRawPcm(rawSound, true);
    ASSERT_NE(raw, 0u);
    EXPECT_GT(pump(32), 0.05f);
    EXPECT_TRUE(engine.stopRawPcm(raw));
    pump(32);
    EXPECT_EQ(engine.unregisterRawPcm(rawSound), HarkenEditor::Bridge::kEngineOk);
}

TEST_F(AuditionLifecycleTest, CanceledAndDeletedPendingTargetsNeverStartAfterPublication)
{
    int started = 0;
    QObject::connect(&audition, &HarkenEditor::AuditionManager::auditionStarted,
                     &audition, [&](uint32_t, const QString&) { ++started; });
    auto& coordinator = engine.runtimeSyncCoordinator();
    {
        coordinator.beginSubmissionBatch();
        const auto release = qScopeGuard([&] { coordinator.endSubmissionBatch(); });
        coordinator.markCurrentWorldMutation();
        const auto canceled = audition.playItem(*action);
        ASSERT_NE(canceled, 0u);
        ASSERT_NE(audition.playItem(*sounds[1]), 0u);
        audition.stop(canceled);
        ASSERT_TRUE(projects.deleteItem(sounds[1]));
        sounds[1] = nullptr;
        EXPECT_FLOAT_EQ(pump(32), 0.0f);
    }
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_FLOAT_EQ(pump(64), 0.0f);
    EXPECT_EQ(started, 0);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
}

TEST_F(AuditionLifecycleTest, PendingStartsCannotCrossRuntimeOrProjectSessions)
{
    int started = 0;
    QObject::connect(&audition, &HarkenEditor::AuditionManager::auditionStarted,
                     &audition, [&](uint32_t, const QString&) { ++started; });
    auto& coordinator = engine.runtimeSyncCoordinator();
    {
        coordinator.beginSubmissionBatch();
        const auto release = qScopeGuard([&] { coordinator.endSubmissionBatch(); });
        coordinator.markCurrentWorldMutation();
        ASSERT_NE(audition.playItem(*sounds[0]), 0u);
        ASSERT_NE(audition.playItem(*action), 0u);
        engine.shutdown();
        HarkenEditor::Bridge::EngineSessionConfig config;
        config.sampleRate = 48000;
        config.bufferSize = 64;
        config.maxVoices = 16;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(engine.initialize(config));
        ASSERT_TRUE(engine.startOutput());
    }
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_FLOAT_EQ(pump(64), 0.0f);
    EXPECT_EQ(started, 0);
    EXPECT_EQ(audition.activeAuditionCount(), 0);

    {
        coordinator.beginSubmissionBatch();
        const auto release = qScopeGuard([&] { coordinator.endSubmissionBatch(); });
        coordinator.markCurrentWorldMutation();
        ASSERT_NE(audition.playItem(*sounds[0]), 0u);
        ASSERT_NE(audition.playItem(*action), 0u);
        ASSERT_TRUE(projects.closeProject());
    }
    QElapsedTimer timeout;
    timeout.start();
    while (projects.isClosing() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        pump(1);
        QThread::msleep(1);
    }
    ASSERT_FALSE(projects.isClosing());
    VNX::ProjectConfig replacement;
    replacement.name = QStringLiteral("ReplacementProject");
    replacement.location = directory.path();
    ASSERT_TRUE(projects.createProject(replacement));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_FLOAT_EQ(pump(64), 0.0f);
    EXPECT_EQ(started, 0);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
}

TEST_F(AuditionLifecycleTest, FailedPublicationCannotFallBackToPreviouslyPlayableDefinitions)
{
    int started = 0;
    QObject::connect(&audition, &HarkenEditor::AuditionManager::auditionStarted,
                     &audition, [&](uint32_t, const QString&) { ++started; });
    auto& coordinator = engine.runtimeSyncCoordinator();
    auto& step = action->comp<VNX::VAction>()->steps.front();
    const auto type = step.type;
    {
        coordinator.beginSubmissionBatch();
        const auto release = qScopeGuard([&] { coordinator.endSubmissionBatch(); });
        coordinator.markCurrentWorldMutation();
        ASSERT_NE(audition.playItem(*sounds[0]), 0u);
        ASSERT_NE(audition.playItem(*action), 0u);
        step.type = static_cast<VNX::VActionStep::Type>(255);
        coordinator.markCurrentWorldMutation();
    }
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);
    ASSERT_TRUE(coordinator.hasInstalledWorld());
    ASSERT_TRUE(engine.hasActionProgram(action->runtimeId()));
    EXPECT_EQ(audition.playItem(*sounds[0]), 0u);
    EXPECT_EQ(audition.playItem(*action), 0u);
    EXPECT_EQ(engine.playByNameDetailed(action->name).playbackHandle, 0u);
    EXPECT_EQ(engine.playByName3DDetailed(action->name, {}).playbackHandle, 0u);
    EXPECT_EQ(engine.playPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableSound,
                                         sounds[0]->runtimeId()).handle, 0u);
    EXPECT_FLOAT_EQ(pump(64), 0.0f);
    EXPECT_EQ(started, 0);
    EXPECT_EQ(audition.activeAuditionCount(), 0);

    step.type = type;
    coordinator.markCurrentWorldMutation();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_FLOAT_EQ(pump(64), 0.0f);
    EXPECT_EQ(started, 0);
    expectGoodAction();
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
