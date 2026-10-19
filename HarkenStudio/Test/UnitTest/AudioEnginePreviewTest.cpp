#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/StudioWorldTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Engine.h"
#include "ProjectGenerationTransaction.h"
#include "Support/RuntimeIdentityTestUtils.h"

#include "AudioEngine.h"
#include "AttenuationStore.h"
#include "BindingEditing.h"
#include "CanonicalPackSetExport.h"
#include "CompileTargetProfileCatalog.h"
#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/MediaPool.h"
#include "CurrentWorldProjectAdapter.h"
#include "Components/VSound.h"
#include "Components/VSpatial.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "Components/VPiece.h"
#include "Components/VTrack.h"
#include "Components/VMixNode.h"
#include "MemorySettingsStorage.h"
#include "PlatformContextService.h"
#include "FieldGroupProjectAdapter.h"
#include "IPC/IPCServer.h"
#include "LanguageContextService.h"
#include "MemorySettingsStorage.h"
#include "ProjectManager.h"
#include "PropertyEditSession.h"
#include "ProjectPackExporter.h"
#include "RuntimeCapabilityMatrix.h"
#include "Components/VAction.h"
#include "ProjectTypes.h"
#include "RuntimeSyncCoordinator.h"
#include "SoundSourceEditing.h"
#include "SoundSourceSemantics.h"
#include "StartupCoordinator.h"
#include "AuditionManager.h"
#include "UndoCommands.h"
#include "Dialogs/StartupLoadingWindow.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/CurveEditor/CurveEditorWidget.h"
#include "Widgets/Blueprint/NodeContentEditor.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/FieldGroupInspectorView.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/ProjectSyncBridge.h"
#include "RuntimeBridge/RuntimeLink.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "UnitTest/CanonicalPackSetActivate.h"

#include "harken/Harken.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/media/PackLogicImage.h"
#include <harken_bus_compiler/BusCompiler.h>

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

QJsonObject gridSelectionsJson(const VNX::VSelection& selection, const QJsonValue& value)
{
    auto selections = selection.selections;
    if (value.isArray()) {
        const auto mixing = VNX::MixingDef::fromJson(value.toArray());
        if (!mixing || !selections.shared.setMixing(*mixing)) return {};
    } else if (!selections.shared.setMatchGrid(VNX::DiscreteGridDef::fromJson(value.toObject()))) {
        return {};
    }
    return selections.toJson();
}

constexpr uint32_t kPreviewSound = 0;
constexpr uint32_t kPreviewContainer = 1;
constexpr uint32_t kPreviewTrack = 2;
constexpr uint32_t kPreviewPiece = 3;
constexpr int32_t kPreviewOk = 0;
constexpr int32_t kPreviewStaleHandle = 5;
constexpr uint32_t kPreviewLogicalComplete = 4;
constexpr uint32_t kPreviewAudibleRetiring = 5;

bool pendingDiscardObserved = false;
bool runtimeActiveDuringPendingDiscard = false;
HarkenEditor::Bridge::CurrentWorldSyncToken discardedPendingToken;
std::size_t serviceInstallCallCount = 0;
std::size_t observedWorldInstallCallCount = 0;
bool emptyWorldInstallObserved = false;
uint32_t runtimeObjectsWhenEmptyWorldInstalled = 0;
std::size_t installCallCountWhenPendingDiscard = 0;
harkenstudiobuild::WorldInstallResult backpressuredWorldInstall(
    const harkenstudiobuild::ContainerWorldInstallViewV1&) noexcept
{
    harkenstudiobuild::WorldInstallResult result;
    result.status = harkenstudiobuild::WorldInstallStatus::Backpressure;
    return result;
}

harkenstudiobuild::WorldInstallResult failedWorldInstall(
    const harkenstudiobuild::ContainerWorldInstallViewV1&) noexcept
{
    harkenstudiobuild::WorldInstallResult result;
    result.status = harkenstudiobuild::WorldInstallStatus::InvalidWorld;
    return result;
}

harkenstudiobuild::WorldInstallResult terminalOnServiceWorldInstall(
    const harkenstudiobuild::ContainerWorldInstallViewV1&) noexcept
{
    harkenstudiobuild::WorldInstallResult result;
    if (serviceInstallCallCount++ == 0) {
        result.status = harkenstudiobuild::WorldInstallStatus::Backpressure;
    } else {
        result.status = harkenstudiobuild::WorldInstallStatus::Ok;
        result.resultGeneration = 111;
    }
    return result;
}

harkenstudiobuild::WorldInstallResult observeEmptyWorldInstall(
    const harkenstudiobuild::ContainerWorldInstallViewV1& world) noexcept
{
    if (world.variableCount == 0 && world.playablePropertyCount == 0
        && world.playableOccurrenceCount == 0 && world.containerCount == 0
        && world.transitionCount == 0) {
        emptyWorldInstallObserved = true;
        runtimeObjectsWhenEmptyWorldInstalled =
            HarkenEditor::Bridge::runtimeObjectCount();
    }
    harken::CurrentWorldBodyImage image;
    harkenstudiobuild::WorldInstallResult result;
    if (!harkentest::DecodeStudioWorld(world, image)) {
        result.status = harkenstudiobuild::WorldInstallStatus::InvalidWorld;
        return result;
    }
    const auto installed = harken::InstallContainerWorldV1(*image.ProductionView());
    result.status = static_cast<harkenstudiobuild::WorldInstallStatus>(installed.status);
    result.priorGeneration = installed.priorGeneration;
    result.resultGeneration = installed.resultGeneration;
    return result;
}

harkenstudiobuild::WorldInstallResult countWorldInstall(
    const harkenstudiobuild::ContainerWorldInstallViewV1& world) noexcept
{
    ++observedWorldInstallCallCount;
    harken::CurrentWorldBodyImage image;
    harkenstudiobuild::WorldInstallResult result;
    if (!harkentest::DecodeStudioWorld(world, image)) {
        result.status = harkenstudiobuild::WorldInstallStatus::InvalidWorld;
        return result;
    }
    const auto installed = harken::InstallContainerWorldV1(*image.ProductionView());
    result.status = static_cast<harkenstudiobuild::WorldInstallStatus>(installed.status);
    result.priorGeneration = installed.priorGeneration;
    result.resultGeneration = installed.resultGeneration;
    return result;
}

harkenstudiobuild::WorldInstallResult countBackpressuredWorldInstall(
    const harkenstudiobuild::ContainerWorldInstallViewV1&) noexcept
{
    ++observedWorldInstallCallCount;
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
    installCallCountWhenPendingDiscard = observedWorldInstallCallCount;
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

struct ContainerActionReset {
    explicit ContainerActionReset(VNX::ProjectManager& projects)
    {
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(
            VNX::ActionManager::instance(), &projects, nullptr, {});
        VNX::registerContainerActions(VNX::ActionManager::instance(), &projects);
        VNX::UndoRedoManager::instance().clear();
    }

    ~ContainerActionReset()
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
    }
};

struct VariableControlPreviewFixture {
    VNX::ProjectItem* container = nullptr;
    VNX::ProjectItem* blend = nullptr;
    VNX::ProjectItem* first = nullptr;
    VNX::ProjectItem* second = nullptr;
    VNX::ProjectItem* step = nullptr;
    VNX::ProjectItem* smooth = nullptr;
};

VariableControlPreviewFixture createVariableControlPreviewFixture(
    VNX::ProjectManager& projects, uint32_t sourceId,
    uint32_t frameCount = 65536, bool nestedBlend = false)
{
    const QString audioSourcesPath = QDir(
        QFileInfo(projects.currentProject()->projectPath).absolutePath())
            .filePath(QStringLiteral("AudioSources"));
    const QString firstWavPath = QDir(audioSourcesPath).filePath(
        QStringLiteral("variable-control-first.wav"));
    const QString secondWavPath = QDir(audioSourcesPath).filePath(
        QStringLiteral("variable-control-second.wav"));
    writeWavF32(firstWavPath, std::vector<float>(frameCount, 0.125f));
    writeWavF32(secondWavPath, std::vector<float>(frameCount, 0.75f));

    VNX::AssetInfo firstAsset;
    firstAsset.sourceID = sourceId;
    firstAsset.relativePath = QStringLiteral("variable-control-first.wav");
    firstAsset.absolutePath = firstWavPath;
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(firstAsset));
    VNX::AssetInfo secondAsset;
    secondAsset.sourceID = sourceId ^ 0x40000000u;
    secondAsset.relativePath = QStringLiteral("variable-control-second.wav");
    secondAsset.absolutePath = secondWavPath;
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(secondAsset));

    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    EXPECT_NE(layer, nullptr);
    if (layer == nullptr)
        return {};

    VariableControlPreviewFixture fixture;
    fixture.container = projects.createItem(
        layer, QStringLiteral("VariableControl"),
        VNX::ProjectItem::Type::Container);
    fixture.first = projects.createAudioItem(
        fixture.container, QStringLiteral("First"), firstWavPath,
        firstAsset.sourceID);
    fixture.second = projects.createAudioItem(
        fixture.container, QStringLiteral("Second"), secondWavPath,
        secondAsset.sourceID);
    fixture.step = projects.createItem(
        nullptr, QStringLiteral("Step"), VNX::ProjectItem::Type::Variable);
    fixture.smooth = projects.createItem(
        nullptr, QStringLiteral("Smooth"), VNX::ProjectItem::Type::Variable);
    EXPECT_NE(fixture.container, nullptr);
    EXPECT_NE(fixture.first, nullptr);
    EXPECT_NE(fixture.second, nullptr);
    EXPECT_NE(fixture.step, nullptr);
    EXPECT_NE(fixture.smooth, nullptr);
    if (fixture.container == nullptr || fixture.first == nullptr
        || fixture.second == nullptr || fixture.step == nullptr
        || fixture.smooth == nullptr) {
        return {};
    }

    auto* selection = fixture.container->comp<VNX::VSelection>();
    auto* step = fixture.step->comp<VNX::VVariable>();
    auto* smooth = fixture.smooth->comp<VNX::VVariable>();
    EXPECT_NE(selection, nullptr);
    EXPECT_NE(step, nullptr);
    EXPECT_NE(smooth, nullptr);
    if (selection == nullptr || step == nullptr || smooth == nullptr)
        return {};

    selection->mode = VNX::SelectionMode::Sequence;
    step->kind = VNX::VariableKind::Step;
    step->rangeMin = 0.0f;
    step->rangeMax = 2.0f;
    step->defaultValue = 1.0f;
    smooth->kind = VNX::VariableKind::Smooth;
    smooth->rangeMin = 0.0f;
    smooth->rangeMax = 1.0f;
    smooth->defaultValue = 0.5f;
    if (nestedBlend) {
        fixture.blend = projects.createItem(
            fixture.container, QStringLiteral("Blend"), VNX::ProjectItem::Type::Container);
        EXPECT_NE(fixture.blend, nullptr);
        if (!fixture.blend)
            return {};
        if (!projects.moveItem(fixture.first, fixture.blend)
            || !projects.moveItem(fixture.second, fixture.blend)) {
            ADD_FAILURE() << "Failed to place preview members inside Blend";
            return {};
        }
        auto* blendSelection = fixture.blend->comp<VNX::VSelection>();
        blendSelection->mode = VNX::SelectionMode::Blend;
        blendSelection->blendResponse = VNX::VariableResponse::Live;
        {
            auto mixing = blendSelection->mixing();
            mixing.rows.push_back({fixture.smooth->guid, {}});
            mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(fixture.first->guid, 0.0f, 100.0f));
            mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(fixture.second->guid, 0.0f, 100.0f));
            mixing.rows.front().children[0].normalize();
            EXPECT_TRUE(blendSelection->selections.shared.setMixing(mixing));
        }
    }
    return fixture;
}

QString guidString(const VNX::ProjectItem& item)
{
    return item.guid.toString(QUuid::WithoutBraces);
}

QJsonValue discreteGridJson(const VariableControlPreviewFixture& fixture)
{
    const QJsonObject discrete{
        {QStringLiteral("axes"), QJsonArray{guidString(*fixture.step)}},
        {QStringLiteral("rows"), QJsonArray{
            QJsonObject{
                {QStringLiteral("target"), guidString(*fixture.first)},
                {QStringLiteral("cells"), QJsonArray{1}}},
            QJsonObject{
                {QStringLiteral("target"), guidString(*fixture.second)},
                {QStringLiteral("cells"), QJsonArray{2}}}}}};
    return discrete;
}

QJsonValue continuouslyEvaluatedDiscreteGridJson(
    const VariableControlPreviewFixture& fixture)
{
    fixture.container->comp<VNX::VSelection>()->matchResponse = VNX::VariableResponse::Live;
    return discreteGridJson(fixture);
}

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

float pumpUntilPreviewPeakAbove(float threshold, uint32_t maxBlocks = 256)
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
        if (peak > threshold)
            return peak;
    }
    return 0.0f;
}

float pumpUntilPreviewPeakInRange(float minimum, float maximum,
                                  uint32_t maxBlocks = 256)
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
        if (peak > minimum && peak < maximum)
            return peak;
    }
    return 0.0f;
}

QJsonValue blendsJson(const VariableControlPreviewFixture& fixture)
{
    return VNX::MixingDef{{{fixture.smooth->guid,
        {VNX::MixingChildDef::unity(fixture.second->guid, 0.0f, 1.0f)}}}}.toJson();
}

QJsonValue defaultBlendJson(const VariableControlPreviewFixture& fixture)
{
    return VNX::MixingDef{{{{}, {{fixture.second->guid, {}, {}}}}}}.toJson();
}

QJsonValue nestedMatchGridJson(const VariableControlPreviewFixture& fixture)
{
    return QJsonObject{
        {QStringLiteral("axes"), QJsonArray{guidString(*fixture.step)}},
        {QStringLiteral("rows"), QJsonArray{QJsonObject{
            {QStringLiteral("target"), guidString(*fixture.blend)},
            {QStringLiteral("cells"), QJsonArray{1}}}}}};
}

void expectVariableControlGridMutationFirstIntent(
    const QString& projectName, uint32_t sourceId,
    QJsonValue (*gridJson)(const VariableControlPreviewFixture&),
    VNX::SelectionMode mode = VNX::SelectionMode::Match,
    bool nestedBlend = false)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = projectName;
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(
        projects, sourceId, 65536, nestedBlend);
    ASSERT_NE(fixture.container, nullptr);

    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto revisionBeforeEdit =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();

    auto* selection = fixture.container->comp<VNX::VSelection>();
    selection->mode = mode;
    selection->matchResponse = VNX::VariableResponse::AtStart;
    selection->blendResponse = VNX::VariableResponse::Live;
    ContainerActionReset actions(projects);
    const auto edited = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*selection, gridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(edited.success) << edited.errorMessage.toStdString();

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts](uint32_t, const QString&, int32_t) {
            ++failedStarts;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    EXPECT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_EQ(successfulStarts, 1);
    EXPECT_TRUE(engine.hasInstalledCurrentWorld());
    const auto revisionAfterEdit =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();
    EXPECT_EQ(revisionAfterEdit, revisionBeforeEdit + 1);
    EXPECT_EQ(failedStarts, 0);
    audition.stopAll(0);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
}

enum class ContainerPreviewShape {
    RandomDraw,
    RandomShuffle,
    RandomQueue,
    Sequence,
    SequenceQueue,
    VariableDiscrete,
    VariableContinuous,
};

bool previewUsesQueue(ContainerPreviewShape shape)
{
    return shape == ContainerPreviewShape::RandomQueue
        || shape == ContainerPreviewShape::SequenceQueue;
}

bool previewUsesVariableControl(ContainerPreviewShape shape)
{
    return shape == ContainerPreviewShape::VariableDiscrete
        || shape == ContainerPreviewShape::VariableContinuous;
}

void proveDirectPreviewStart(const VNX::ProjectItem& container,
                             quint64 previewHandle)
{
    ASSERT_NE(previewHandle, 0u);
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    constexpr quint64 kPreviewHandleTag = quint64{1} << 63;
    const quint64 internal = previewHandle & ~kPreviewHandleTag;
    const auto row = std::find_if(
        runtime->playableServices.begin(), runtime->playableServices.end(),
        [&](const harken::EnginePlayableServiceRow& candidate) {
            return candidate.phase != harken::EnginePlayableStartPhase::Free
                && (candidate.rootInstance == internal
                    || candidate.root.stableId == container.runtimeId());
        });
    ASSERT_NE(row, runtime->playableServices.end());
    EXPECT_EQ(row->Invocation().packLease.leaseToken, 0u);
    EXPECT_EQ(row->Invocation().actionFiring, 0u);
    EXPECT_EQ(row->root.kind, harken::PlayableKindV1::Container);
}

bool queueLeafAdvanced(quint64 previewHandle, uint32_t maxBlocks)
{
    auto* runtime = harken::detail::ActiveEngine();
    if (runtime == nullptr || previewHandle == 0)
        return false;
    constexpr quint64 kPreviewHandleTag = quint64{1} << 63;
    const quint64 internal = previewHandle & ~kPreviewHandleTag;
    harkenstudio::ObjectId firstLeaf{};
    for (uint32_t block = 0; block < maxBlocks; ++block) {
        if (Vn_Update() != VN_OK)
            return false;
        runtime->nullBackend.Pump(1);
        for (const auto& row : runtime->playableServices) {
            if (row.rootInstance != internal || row.leaf.stableId == harkentest::StudioObjectId(0))
                continue;
            if (harkenstudio::IsNull(firstLeaf)) {
                firstLeaf = harkentest::ToStudio(row.leaf.stableId);
                continue;
            }
            if (row.leaf.stableId != firstLeaf)
                return true;
        }
    }
    return false;
}

void expectContainerFamilyPreviewStarts(
    const QString& projectName, uint32_t sourceId,
    ContainerPreviewShape shape)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = projectName;
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));

    VNX::ProjectItem* container = nullptr;
    if (previewUsesVariableControl(shape)) {
        const auto fixture = createVariableControlPreviewFixture(
            projects, sourceId);
        ASSERT_NE(fixture.container, nullptr);
        container = fixture.container;
        auto* selection = container->comp<VNX::VSelection>();
        ASSERT_NE(selection, nullptr);
        selection->mode = VNX::SelectionMode::Match;
        if (shape == ContainerPreviewShape::VariableDiscrete) {
            {
                auto grid = selection->matchGrid();
                ASSERT_TRUE(grid.addAxis(fixture.step->guid));
                ASSERT_TRUE(grid.addRow(fixture.first->guid));
                ASSERT_TRUE(grid.addRow(fixture.second->guid));
                ASSERT_TRUE(grid.setCell(0, 0, 1));
                ASSERT_TRUE(grid.setCell(1, 0, 2));
                EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
            }
        } else {
            selection->mode = VNX::SelectionMode::Blend;
            selection->blendResponse = VNX::VariableResponse::Live;
            auto* smooth = fixture.smooth->comp<VNX::VVariable>();
            ASSERT_NE(smooth, nullptr);
            smooth->defaultValue = 0.875f;
            {
                auto mixing = selection->mixing();
                mixing.rows.push_back({
                    fixture.smooth->guid, {}});
                mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(fixture.first->guid, 0.0f, 100.0f));
                mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(fixture.second->guid, 0.0f, 100.0f));
                mixing.rows.front().children[0].points = {{0.0f, 1.0f}, {0.25f, 1.0f}, {0.75f, 0.0f}, {1.0f, 0.0f}};
                mixing.rows.front().children[1].points = {{0.0f, 0.0f}, {0.25f, 0.0f}, {0.75f, 1.0f}, {1.0f, 1.0f}};
                for (auto& child : mixing.rows.front().children) child.normalize();
                EXPECT_TRUE(selection->selections.shared.setMixing(mixing));
            }
        }
    } else {
        const QString audioSourcesPath = QDir(
            QFileInfo(projects.currentProject()->projectPath).absolutePath())
                .filePath(QStringLiteral("AudioSources"));
        const QString firstPath = QDir(audioSourcesPath).filePath(
            QStringLiteral("family-first.wav"));
        const QString secondPath = QDir(audioSourcesPath).filePath(
            QStringLiteral("family-second.wav"));
        const uint32_t childFrames =
            previewUsesQueue(shape) ? 512u : 65536u;
        writeWavF32(firstPath, std::vector<float>(childFrames, 0.125f));
        writeWavF32(secondPath, std::vector<float>(childFrames, 0.75f));
        VNX::AssetInfo firstAsset;
        firstAsset.sourceID = sourceId;
        firstAsset.relativePath = QStringLiteral("family-first.wav");
        firstAsset.absolutePath = firstPath;
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(firstAsset));
        VNX::AssetInfo secondAsset;
        secondAsset.sourceID = sourceId ^ 0x40000000u;
        secondAsset.relativePath = QStringLiteral("family-second.wav");
        secondAsset.absolutePath = secondPath;
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(secondAsset));
        auto* layer = projects.createItem(
            nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        container = projects.createItem(
            layer, QStringLiteral("Container"),
            VNX::ProjectItem::Type::Container);
        ASSERT_NE(container, nullptr);
        ASSERT_NE(projects.createAudioItem(
            container, QStringLiteral("First"), firstPath,
            firstAsset.sourceID), nullptr);
        ASSERT_NE(projects.createAudioItem(
            container, QStringLiteral("Second"), secondPath,
            secondAsset.sourceID), nullptr);
        auto* selection = container->comp<VNX::VSelection>();
        ASSERT_NE(selection, nullptr);
        switch (shape) {
        case ContainerPreviewShape::RandomDraw:
            selection->mode = VNX::SelectionMode::Random;
            selection->behavior.discipline = VNX::RandomDiscipline::Draw;
            break;
        case ContainerPreviewShape::RandomShuffle:
            selection->mode = VNX::SelectionMode::Random;
            selection->behavior.discipline = VNX::RandomDiscipline::Shuffle;
            break;
        case ContainerPreviewShape::RandomQueue:
            selection->mode = VNX::SelectionMode::Random;
            selection->playType = VNX::SelectionPlayType::Queue;
            break;
        case ContainerPreviewShape::Sequence:
            selection->mode = VNX::SelectionMode::Sequence;
            break;
        case ContainerPreviewShape::SequenceQueue:
            selection->mode = VNX::SelectionMode::Sequence;
            selection->playType = VNX::SelectionPlayType::Queue;
            break;
        case ContainerPreviewShape::VariableDiscrete:
        case ContainerPreviewShape::VariableContinuous:
            break;
        }
    }
    ASSERT_NE(container, nullptr);

    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    int32_t failureResult = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts, &failureResult](uint32_t, const QString&, int32_t result) {
            ++failedStarts;
            failureResult = result;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });
    const uint32_t playingId = audition.playItem(*container);
    ASSERT_NE(playingId, 0u) << "engine result "
        << static_cast<int>(audition.lastPlayEngineResult());
    EXPECT_NE(audition.lastPlayEngineResult(),
              HarkenEditor::Bridge::kEngineUnknownId);
    const quintptr previewHandle =
        audition.getAudition(playingId).playbackHandle;
    ASSERT_NE(previewHandle, 0u);
    EXPECT_EQ(successfulStarts, 1);
    EXPECT_EQ(failedStarts, 0) << "engine result " << failureResult;
    ASSERT_NO_FATAL_FAILURE(proveDirectPreviewStart(*container, previewHandle));
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.05f);
    if (previewUsesQueue(shape)) {
        EXPECT_TRUE(queueLeafAdvanced(previewHandle, 128));
    }
}

struct SequentialPreviewTarget {
    QString name;
    VNX::ProjectItem* container = nullptr;
    bool queue = false;
};

void pumpPreviewTransport(VNX::AudioEngine& engine,
                          HarkenEditor::AuditionManager& audition)
{
    auto* runtime = harken::detail::ActiveEngine();
    if (runtime != nullptr) {
        Vn_Update();
        runtime->nullBackend.Pump(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QMetaObject::invokeMethod(
        &engine, "onRuntimeServiceTimer", Qt::DirectConnection);
    QMetaObject::invokeMethod(
        &audition, "pollAuditionStates", Qt::DirectConnection);
    QMetaObject::invokeMethod(
        &audition, "retryPendingPreviewStarts", Qt::DirectConnection);
}

bool typedPreviewStopStuck(const HarkenEditor::AuditionManager& audition)
{
    for (const auto& info : audition.getActiveAuditions()) {
        if (info.mode == HarkenEditor::AuditionMode::Preview
            && info.previewStopStuck)
            return true;
    }
    return false;
}

bool typedPreviewSlotOccupied(const HarkenEditor::AuditionManager& audition)
{
    for (const auto& info : audition.getActiveAuditions()) {
        if (info.mode == HarkenEditor::AuditionMode::Preview)
            return true;
    }
    return false;
}

bool waitUntilTypedPreviewRetired(
    VNX::AudioEngine& engine, HarkenEditor::AuditionManager& audition)
{
    for (int attempt = 0; attempt < 256; ++attempt) {
        if (typedPreviewStopStuck(audition))
            return false;
        if (!typedPreviewSlotOccupied(audition))
            return true;
        pumpPreviewTransport(engine, audition);
    }
    return !typedPreviewSlotOccupied(audition)
        && !typedPreviewStopStuck(audition);
}

bool previewPlayAccepted(const HarkenEditor::AuditionInfo& info)
{
    return info.playingId != 0
        && !info.previewReplacePending
        && !info.previewCancelling
        && !info.previewStopPending
        && info.bIsPlaying
        && info.playbackHandle != 0
        && info.previewReason.isEmpty();
}

void expectAllContainerModesPlaySequentially(const QString& projectLocation,
                                             bool saveAfterCreate)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QDir().mkpath(projectLocation);
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("five_containers");
    projectConfig.location = projectLocation;
    projectConfig.platforms = HarkenTest::platforms(QStringList{QStringLiteral("Windows")});
    ASSERT_TRUE(projects.createProject(projectConfig));

    const QString audioSourcesPath = QDir(
        QFileInfo(projects.currentProject()->projectPath).absolutePath())
            .filePath(QStringLiteral("AudioSources"));
    const QString firstPath = QDir(audioSourcesPath).filePath(
        QStringLiteral("family-first.wav"));
    const QString secondPath = QDir(audioSourcesPath).filePath(
        QStringLiteral("family-second.wav"));
    writeWavF32(firstPath, std::vector<float>(512, 0.125f));
    writeWavF32(secondPath, std::vector<float>(512, 0.75f));
    VNX::AssetInfo firstAsset;
    firstAsset.sourceID = 7401;
    firstAsset.relativePath = QStringLiteral("family-first.wav");
    firstAsset.absolutePath = firstPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(firstAsset));
    VNX::AssetInfo secondAsset;
    secondAsset.sourceID = 7401u ^ 0x40000000u;
    secondAsset.relativePath = QStringLiteral("family-second.wav");
    secondAsset.absolutePath = secondPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(secondAsset));

    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Containers"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);

    const struct {
        const char* name;
        ContainerPreviewShape shape;
    } kModes[] = {
        {"Random_Draw", ContainerPreviewShape::RandomDraw},
        {"Random_Shuffle", ContainerPreviewShape::RandomShuffle},
        {"Random_Queue", ContainerPreviewShape::RandomQueue},
        {"Sequence", ContainerPreviewShape::Sequence},
        {"Sequence_Queue", ContainerPreviewShape::SequenceQueue},
        {"Variable_Control", ContainerPreviewShape::VariableDiscrete},
    };

    std::vector<SequentialPreviewTarget> targets;
    VNX::ProjectItem* step = nullptr;
    for (const auto& mode : kModes) {
        SequentialPreviewTarget target;
        target.name = QString::fromLatin1(mode.name);
        target.queue = previewUsesQueue(mode.shape);
        if (mode.shape == ContainerPreviewShape::VariableDiscrete) {
            const auto fixture = createVariableControlPreviewFixture(
                projects, 7410);
            ASSERT_NE(fixture.container, nullptr);
            fixture.container->name = target.name;
            target.container = fixture.container;
            step = fixture.step;
            auto* selection = target.container->comp<VNX::VSelection>();
            ASSERT_NE(selection, nullptr);
            selection->mode = VNX::SelectionMode::Match;
            {
                auto grid = selection->matchGrid();
                ASSERT_TRUE(grid.addAxis(fixture.step->guid));
                ASSERT_TRUE(grid.addRow(fixture.first->guid));
                ASSERT_TRUE(grid.addRow(fixture.second->guid));
                ASSERT_TRUE(grid.setCell(0, 0, 1));
                ASSERT_TRUE(grid.setCell(1, 0, 2));
                EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
            }
        } else {
            target.container = projects.createItem(
                layer, target.name, VNX::ProjectItem::Type::Container);
            ASSERT_NE(target.container, nullptr);
            ASSERT_NE(projects.createAudioItem(
                target.container, target.name + QStringLiteral("_Boar"),
                firstPath, firstAsset.sourceID), nullptr);
            ASSERT_NE(projects.createAudioItem(
                target.container, target.name + QStringLiteral("_Gacha"),
                secondPath, secondAsset.sourceID), nullptr);
            auto* selection = target.container->comp<VNX::VSelection>();
            ASSERT_NE(selection, nullptr);
            switch (mode.shape) {
            case ContainerPreviewShape::RandomDraw:
                selection->mode = VNX::SelectionMode::Random;
                selection->behavior.discipline = VNX::RandomDiscipline::Draw;
                break;
            case ContainerPreviewShape::RandomShuffle:
                selection->mode = VNX::SelectionMode::Random;
                selection->behavior.discipline = VNX::RandomDiscipline::Shuffle;
                break;
            case ContainerPreviewShape::RandomQueue:
                selection->mode = VNX::SelectionMode::Random;
                selection->playType = VNX::SelectionPlayType::Queue;
                break;
            case ContainerPreviewShape::Sequence:
                selection->mode = VNX::SelectionMode::Sequence;
                break;
            case ContainerPreviewShape::SequenceQueue:
                selection->mode = VNX::SelectionMode::Sequence;
                selection->playType = VNX::SelectionPlayType::Queue;
                break;
            default:
                break;
            }
        }
        targets.push_back(target);
    }
    ASSERT_EQ(targets.size(), 6u);
    Q_UNUSED(step);

    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    if (saveAfterCreate) {
        ASSERT_TRUE(projects.saveProject());
        VNX::AssetRegistry::instance().markAllCommitted();
        ASSERT_TRUE(QFile::exists(firstPath));
        ASSERT_TRUE(QFile::exists(secondPath));
    }

    HarkenEditor::AuditionManager audition(&engine);
    for (const auto& target : targets) {
        ASSERT_NE(target.container, nullptr) << target.name.toStdString();
        audition.stopAll(0.0f);
        ASSERT_TRUE(waitUntilTypedPreviewRetired(engine, audition))
            << target.name.toStdString()
            << " stop-stuck=" << typedPreviewStopStuck(audition)
            << " reason=" << audition.lastPlayReason().toStdString();
        ASSERT_FALSE(typedPreviewStopStuck(audition)) << target.name.toStdString();
        ASSERT_TRUE(serviceUntilCurrentWorldReady(engine)) << target.name.toStdString();

        int failedStarts = 0;
        int successfulStarts = 0;
        int32_t failureResult = 0;
        QMetaObject::Connection failConn = QObject::connect(
            &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
            [&](uint32_t, const QString&, int32_t result) {
                ++failedStarts;
                failureResult = result;
            });
        QMetaObject::Connection startConn = QObject::connect(
            &audition, &HarkenEditor::AuditionManager::auditionStarted,
            [&](uint32_t, const QString&) { ++successfulStarts; });

        const uint32_t playingId = audition.playItem(*target.container);
        ASSERT_NE(playingId, 0u) << target.name.toStdString()
            << " engine result "
            << static_cast<int>(audition.lastPlayEngineResult())
            << " reason " << audition.lastPlayReason().toStdString();
        HarkenEditor::AuditionInfo info = audition.getAudition(playingId);
        for (int attempt = 0; attempt < 256 && !previewPlayAccepted(info)
             && failedStarts == 0; ++attempt) {
            pumpPreviewTransport(engine, audition);
            info = audition.getAudition(playingId);
        }
        EXPECT_EQ(failedStarts, 0) << target.name.toStdString()
            << " engine result " << failureResult;
        ASSERT_TRUE(previewPlayAccepted(info)) << target.name.toStdString()
            << " replacePending=" << info.previewReplacePending
            << " cancelling=" << info.previewCancelling
            << " playing=" << info.bIsPlaying
            << " handle=" << info.playbackHandle
            << " reason=" << info.previewReason.toStdString();
        EXPECT_EQ(info.objectName, target.container->name) << target.name.toStdString();
        const quintptr previewHandle = info.playbackHandle;
        EXPECT_NE(audition.lastPlayEngineResult(),
                  HarkenEditor::Bridge::kEngineUnknownId)
            << target.name.toStdString();
        ASSERT_NO_FATAL_FAILURE(
            proveDirectPreviewStart(*target.container, previewHandle))
            << target.name.toStdString();
        EXPECT_GT(pumpUntilPreviewAudible(64), 0.05f) << target.name.toStdString();
        if (target.queue)
            EXPECT_TRUE(queueLeafAdvanced(previewHandle, 128))
                << target.name.toStdString();
        QObject::disconnect(failConn);
        QObject::disconnect(startConn);
        audition.stopAll(0.0f);
        ASSERT_TRUE(waitUntilTypedPreviewRetired(engine, audition))
            << target.name.toStdString();
    }
}

struct DualSequencePreview {
    VNX::ProjectItem* first = nullptr;
    VNX::ProjectItem* second = nullptr;
};

DualSequencePreview createDualSequencePreview(
    VNX::ProjectManager& projects, uint32_t sourceId)
{
    DualSequencePreview fixture;
    const QString audioSourcesPath = QDir(
        QFileInfo(projects.currentProject()->projectPath).absolutePath())
            .filePath(QStringLiteral("AudioSources"));
    const QString firstPath = QDir(audioSourcesPath).filePath(
        QStringLiteral("transport-first.wav"));
    const QString secondPath = QDir(audioSourcesPath).filePath(
        QStringLiteral("transport-second.wav"));
    writeWavF32(firstPath, std::vector<float>(65536, 0.125f));
    writeWavF32(secondPath, std::vector<float>(65536, 0.75f));
    VNX::AssetInfo firstAsset;
    firstAsset.sourceID = sourceId;
    firstAsset.relativePath = QStringLiteral("transport-first.wav");
    firstAsset.absolutePath = firstPath;
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(firstAsset));
    VNX::AssetInfo secondAsset;
    secondAsset.sourceID = sourceId ^ 0x40000000u;
    secondAsset.relativePath = QStringLiteral("transport-second.wav");
    secondAsset.absolutePath = secondPath;
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(secondAsset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("TransportLayer"),
        VNX::ProjectItem::Type::Layer);
    EXPECT_NE(layer, nullptr);
    if (layer == nullptr)
        return fixture;
    fixture.first = projects.createItem(
        layer, QStringLiteral("TransportFirst"),
        VNX::ProjectItem::Type::Container);
    fixture.second = projects.createItem(
        layer, QStringLiteral("TransportSecond"),
        VNX::ProjectItem::Type::Container);
    EXPECT_NE(fixture.first, nullptr);
    EXPECT_NE(fixture.second, nullptr);
    if (fixture.first == nullptr || fixture.second == nullptr)
        return {};
    EXPECT_NE(projects.createAudioItem(
        fixture.first, QStringLiteral("FirstChild"), firstPath,
        firstAsset.sourceID), nullptr);
    EXPECT_NE(projects.createAudioItem(
        fixture.second, QStringLiteral("SecondChild"), secondPath,
        secondAsset.sourceID), nullptr);
    auto* firstSelection = fixture.first->comp<VNX::VSelection>();
    auto* secondSelection = fixture.second->comp<VNX::VSelection>();
    EXPECT_NE(firstSelection, nullptr);
    EXPECT_NE(secondSelection, nullptr);
    if (firstSelection == nullptr || secondSelection == nullptr)
        return {};
    firstSelection->mode = VNX::SelectionMode::Sequence;
    secondSelection->mode = VNX::SelectionMode::Sequence;
    return fixture;
}

int previewHandleCount(const HarkenEditor::AuditionManager& audition)
{
    int count = 0;
    for (const auto& info : audition.getActiveAuditions()) {
        if (info.mode == HarkenEditor::AuditionMode::Preview
            && info.playbackHandle != 0)
            ++count;
    }
    return count;
}

int previewSlotCount(const HarkenEditor::AuditionManager& audition)
{
    int count = 0;
    for (const auto& info : audition.getActiveAuditions()) {
        if (info.mode == HarkenEditor::AuditionMode::Preview)
            ++count;
    }
    return count;
}

} // namespace

TEST(AudioEnginePreview, TypedCommandsPreservePreviewBoundarySemantics)
{
    auto& engine = VNX::AudioEngine::instance();

    const auto start = engine.playPreviewPlayable(
        std::numeric_limits<uint32_t>::max(), harkentest::StudioObjectId(1));
    EXPECT_EQ(start.status, 1);
    EXPECT_EQ(start.handle, 0u);

    VNX::AudioEngine::PreviewPlayableState state;
    state.stableId = harkentest::StudioObjectId(77);
    state.worldGeneration = 88;
    EXPECT_EQ(engine.queryPreviewPlayable(1, state), 1);
    EXPECT_EQ(state.stableId, harkentest::StudioObjectId(77u));
    EXPECT_EQ(state.worldGeneration, 88u);

    EXPECT_EQ(engine.stopPreviewPlayable(1), 1);
}

TEST(AudioEnginePreview, RuntimeServiceCadenceTracksTheAudioBlock)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 4;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));

    auto* serviceTimer = engine.findChild<QTimer*>(QStringLiteral("runtimeServiceTimer"));
    ASSERT_NE(serviceTimer, nullptr);
    EXPECT_TRUE(serviceTimer->isActive());
    EXPECT_EQ(serviceTimer->timerType(), Qt::PreciseTimer);
    EXPECT_EQ(serviceTimer->interval(), 1);

    ASSERT_TRUE(engine.reconfigureBufferSize(512));
    EXPECT_EQ(serviceTimer->interval(), 5);
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

TEST(AudioEnginePreview, RuntimeServiceConsumesOneCurrentWorldCompletion)
{
    ensureApp();
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        terminalOnServiceWorldInstall);
    serviceInstallCallCount = 0;

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    const HarkenEditor::Bridge::CurrentWorldSyncToken token{41, 42, 43, 44};
    auto request = makeValidCurrentWorldRequest();
    ASSERT_TRUE(HarkenEditor::Bridge::syncCurrentWorld(
                    std::move(request), token).pending());

    ASSERT_TRUE(QMetaObject::invokeMethod(
        &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    EXPECT_EQ(2u, serviceInstallCallCount);
    EXPECT_FALSE(HarkenEditor::Bridge::takeCurrentWorldSyncCompletion().has_value());

    engine.shutdown();
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
}

TEST(AudioEnginePreview,
     InvalidReplacementSupersedesPendingWorldAndFailsCurrentBarrier)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    if (projects.currentProject() != nullptr)
        projects.closeProject();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("InvalidPendingReplacement");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* project = projects.currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_FALSE(project->busses.empty());
    ASSERT_NE(project->busses.front().rootItem, nullptr);
    auto& master = *project->busses.front().rootItem;
    const harkenstudio::ObjectId masterStableId = master.runtimeId();
    ASSERT_NE(masterStableId, harkenstudio::kMasterBusObjectId);

    pendingDiscardObserved = false;
    HarkenEditor::Bridge::setCurrentWorldDiscardObserverForTesting(
        observePendingDiscard);
    auto& coordinator = engine.runtimeSyncCoordinator();
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    const auto pending = coordinator.markCurrentWorldMutation();
    ASSERT_EQ(pending.state,
              VNX::RuntimeSyncCoordinator::AllocationState::Allocated);
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(coordinator.barrier(pending.revision).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Pending);

    master.guid = VNX::authoringUuid(harkentest::StudioObjectId(0));
    const auto rejected = coordinator.markCurrentWorldMutation();
    ASSERT_EQ(rejected.state,
              VNX::RuntimeSyncCoordinator::AllocationState::Allocated);
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
    if (const auto superseded = HarkenEditor::Bridge::discardPendingCurrentWorld(
            HarkenEditor::Bridge::PendingCurrentWorldDisposition::Superseded)) {
        coordinator.acceptCurrentWorldSyncResult(*superseded);
    }
    coordinator.flushPendingMutationNow();
    master.guid = VNX::authoringUuid(harkentest::StudioObjectId(masterStableId));

    EXPECT_TRUE(pendingDiscardObserved);
    const auto failed = coordinator.barrier(rejected.revision);
    EXPECT_EQ(failed.state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);
    EXPECT_EQ(failed.failure,
              HarkenEditor::Bridge::CurrentWorldSyncStatus::InvalidProject);

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
    EXPECT_FALSE(HarkenEditor::Bridge::takeCurrentWorldSyncCompletion()
                     .has_value());
    EXPECT_EQ(coordinator.barrier(rejected.revision).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);
}

TEST(AudioEnginePreview, ProjectMutationsReachCoordinatorWithoutIPCServer)
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
    const auto before = engine.runtimeSyncCoordinator().barrier(0);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("CoordinatorOwnedMutationSync");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));

    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* container = projects.createItem(
        layer, QStringLiteral("Container"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    emit projects.itemPropertyChanged(container, QStringLiteral("behavior"));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    const auto after = engine.runtimeSyncCoordinator().barrier(
        engine.runtimeSyncCoordinator().currentAuthoringRevision());
    EXPECT_GT(after.projectSessionId, before.projectSessionId);
    EXPECT_GT(after.requiredRevision, 0u);
    EXPECT_EQ(VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready,
              after.state);
}

TEST(AudioEnginePreview, LanguageSwitchChangesMediaAndEmptyGroupLeavesUnlocalizedPreviewUsable)
{
    ensureApp();
    struct LanguageStorageReset {
        ~LanguageStorageReset()
        {
            VNX::LanguageContextService::instance().setStorageForTesting(
                VNX::SettingsStorageFactory::create(
                    VNX::SettingsStorageFactory::StorageType::QSettings,
                    QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
        }
    } languageStorageReset;
    VNX::LanguageContextService::instance().setStorageForTesting(
        std::make_unique<VNX::MemorySettingsStorage>());
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("LanguagePreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    projects.setProjectLanguagesInternal(
        {QStringLiteral("English"), QStringLiteral("Japanese"), QStringLiteral("French")});
    auto& languages = VNX::LanguageContextService::instance();
    languages.setLanguage(QStringLiteral("English"));
    const QString audioDirectory = QDir(
        QFileInfo(projects.currentProject()->projectPath).absolutePath())
            .filePath(QStringLiteral("AudioSources"));
    std::array<VNX::AssetInfo, 3> assets;
    for (std::size_t index = 0; index < assets.size(); ++index) {
        auto& asset = assets[index];
        asset.sourceID = static_cast<uint32_t>(7690 + index);
        asset.relativePath = QStringLiteral("language-%1.wav").arg(index);
        asset.absolutePath = QDir(audioDirectory).filePath(asset.relativePath);
        asset.metadata.channels = 1;
        asset.metadata.sampleRate = 48000;
        asset.metadata.totalSamples = 65536;
        asset.metadata.durationSec = 65536.0 / 48000.0;
        const float amplitude = index == 0 ? 0.125f : index == 1 ? 0.75f : 0.25f;
        writeWavF32(asset.absolutePath, std::vector<float>(65536, amplitude));
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    }
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* voice = projects.createAudioItem(
        layer, QStringLiteral("Voice"), assets[0].absolutePath, assets[0].sourceID);
    auto* effect = projects.createAudioItem(
        layer, QStringLiteral("Effect"), assets[2].absolutePath, assets[2].sourceID);
    ASSERT_NE(voice, nullptr);
    ASSERT_NE(effect, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    auto& coordinator = engine.runtimeSyncCoordinator();
    const auto unlocalizedRevision = coordinator.currentAuthoringRevision();
    languages.setLanguage(QStringLiteral("Japanese"));
    EXPECT_EQ(coordinator.currentAuthoringRevision(), unlocalizedRevision);
    languages.setLanguage(QStringLiteral("English"));
    EXPECT_EQ(coordinator.currentAuthoringRevision(), unlocalizedRevision);

    ContainerActionReset actions(projects);
    const auto setProperty = [&](const QString& property, const QJsonValue& value) {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            QJsonObject{{QStringLiteral("authoring_handle"), static_cast<qint64>(voice->objectID)},
                        {QStringLiteral("property"), property},
                        {QStringLiteral("value"), value}}, VNX::ActionSource::GUI);
    };
    ASSERT_TRUE(setProperty(QStringLiteral("distinguishLanguage"), true).success);
    auto* sound = voice->comp<VNX::VSound>();
    auto sources = sound->sources;
    auto& english = sources.localized["English"];
    english.entries = {{QUuid::createUuid(), assets[0].sourceID}};
    english.active = english.entries[0].entryId;
    auto& japanese = sources.localized["Japanese"];
    japanese.entries = {{QUuid::createUuid(), assets[1].sourceID}};
    japanese.active = japanese.entries[0].entryId;
    const auto changed = setProperty(QStringLiteral("sources"),
        VNX::SoundSourceSemantics::toJson(sources));
    ASSERT_TRUE(changed.success) << changed.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const QJsonObject authored = sound->toJson();
    const auto sourceId = voice->sourceID;
    const QString sourcePath = voice->sourcePath;
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    HarkenEditor::AuditionManager audition(&engine);
    const auto expectPreviewPeak = [&](VNX::ProjectItem& item, float minimum, float maximum) {
        const uint32_t playingId = audition.playItem(item);
        ASSERT_NE(playingId, 0u) << audition.lastPlayReason().toStdString();
        for (int attempt = 0; attempt < 32
             && audition.getAudition(playingId).playbackHandle == 0; ++attempt)
            pumpPreviewTransport(engine, audition);
        ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);
        const float peak = pumpUntilPreviewAudible();
        EXPECT_GT(peak, minimum);
        EXPECT_LT(peak, maximum);
        audition.stopAll(0.0f);
        ASSERT_TRUE(waitUntilTypedPreviewRetired(engine, audition));
    };
    ASSERT_NO_FATAL_FAILURE(expectPreviewPeak(*voice, 0.1f, 0.2f));
    const auto englishRevision = coordinator.currentAuthoringRevision();
    languages.setLanguage(QStringLiteral("Japanese"));
    EXPECT_GT(coordinator.currentAuthoringRevision(), englishRevision);
    EXPECT_EQ(sound->toJson(), authored);
    EXPECT_EQ(voice->sourceID, assets[1].sourceID);
    EXPECT_EQ(voice->sourcePath, assets[1].relativePath);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(HarkenEditor::Bridge::mediaRegistryFindPath(assets[1].sourceID),
              assets[1].absolutePath);
    ASSERT_NO_FATAL_FAILURE(expectPreviewPeak(*voice, 0.7f, 0.8f));

    languages.setLanguage(QStringLiteral("French"));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(VNX::SoundSourceEditing::activeSourceId(
        *voice, VNX::ProjectPlatformId{}, languages.language()), 0u);
    EXPECT_EQ(audition.playItem(*voice), 0u);
    EXPECT_EQ(sound->toJson(), authored);
    EXPECT_EQ(voice->sourceID, 0u);
    EXPECT_TRUE(voice->sourcePath.isEmpty());
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
    ASSERT_NO_FATAL_FAILURE(expectPreviewPeak(*effect, 0.2f, 0.3f));

    languages.setLanguage(QStringLiteral("English"));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(sound->toJson(), authored);
    EXPECT_EQ(voice->sourceID, sourceId);
    EXPECT_EQ(voice->sourcePath, sourcePath);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
    ASSERT_NO_FATAL_FAILURE(expectPreviewPeak(*voice, 0.1f, 0.2f));

    ASSERT_TRUE(setProperty(QStringLiteral("distinguishLanguage"), false).success);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto disabledRevision = coordinator.currentAuthoringRevision();
    languages.setLanguage(QStringLiteral("Japanese"));
    EXPECT_EQ(coordinator.currentAuthoringRevision(), disabledRevision);
    ASSERT_NO_FATAL_FAILURE(expectPreviewPeak(*voice, 0.1f, 0.2f));
    const QUuid voiceGuid = voice->guid;
    ASSERT_TRUE(projects.saveProject());
    const QString projectPath = projects.currentProject()->projectPath;
    ASSERT_TRUE(projects.loadProject(projectPath));
    for (int attempt = 0; attempt < 32 && projects.currentProject() == nullptr; ++attempt) {
        QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    ASSERT_NE(projects.currentProject(), nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    voice = projects.findItemByGuid(voiceGuid);
    ASSERT_NE(voice, nullptr);
    sound = voice->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    EXPECT_FALSE(sound->distinguishLanguage);
    EXPECT_EQ(sound->sources, sources);
    ASSERT_NO_FATAL_FAILURE(expectPreviewPeak(*voice, 0.1f, 0.2f));
}

TEST(AudioEnginePreview, PreviewWaitsForLatestRevisionWithoutSecondPlayRequest)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("waiting.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("RevisionGatedPreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6991;
    asset.relativePath = QStringLiteral("waiting.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = projects.createAudioItem(
        layer, QStringLiteral("WaitingSound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto& coordinator = engine.runtimeSyncCoordinator();
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    const auto firstRevision = coordinator.markCurrentWorldMutation();
    ASSERT_EQ(VNX::RuntimeSyncCoordinator::AllocationState::Allocated,
              firstRevision.state);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts](uint32_t, const QString&, int32_t) {
            ++failedStarts;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });

    const uint32_t playingId = audition.playItem(*sound);
    ASSERT_NE(playingId, 0u);
    const auto started = audition.getAudition(playingId);
    EXPECT_FALSE(started.revisionPending);
    EXPECT_NE(started.playbackHandle, 0u);
    EXPECT_EQ(1, successfulStarts);
    EXPECT_EQ(0, failedStarts);

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
    EXPECT_EQ(0, failedStarts);
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
}

TEST(AudioEnginePreview, ActionWaitsForLatestRevisionWithoutSecondPlayRequest)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("waiting-action.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("RevisionGatedAction");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6993;
    asset.relativePath = QStringLiteral("waiting-action.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = projects.createAudioItem(
        layer, QStringLiteral("WaitingActionSound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);
    auto* action = projects.createActionTargeting(
        sound, VNX::VActionStep::Type::Play,
        QStringLiteral("WaitingAction"));
    ASSERT_NE(action, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    const auto pendingRevision = engine.runtimeSyncCoordinator()
        .markCurrentWorldMutation();
    ASSERT_EQ(pendingRevision.state,
              VNX::RuntimeSyncCoordinator::AllocationState::Allocated);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    const auto direct = engine.playByNameDetailed(action->name);
    EXPECT_NE(direct.playbackHandle, 0u);
    EXPECT_EQ(direct.engineResult, HarkenEditor::Bridge::kEngineOk);

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts](uint32_t, const QString&, int32_t) {
            ++failedStarts;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });

    const uint32_t playingId = audition.playItem(*action);
    ASSERT_NE(playingId, 0u);
    const auto started = audition.getAudition(playingId);
    EXPECT_FALSE(started.revisionPending);
    EXPECT_NE(started.playbackHandle, 0u);
    EXPECT_EQ(successfulStarts, 1);
    EXPECT_EQ(failedStarts, 0);
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
}

TEST(AudioEnginePreview, PendingActionRenameStartsCurrentActionIdentity)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath = QDir(directory.path()).filePath(
        QStringLiteral("renamed-action.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("RenamedPendingAction");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6994;
    asset.relativePath = QStringLiteral("renamed-action.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    const QString originalName = QStringLiteral("PendingActionName");
    auto* decoy = projects.createAudioItem(
        layer, originalName, wavPath, asset.sourceID);
    ASSERT_NE(decoy, nullptr);
    auto* target = projects.createAudioItem(
        layer, QStringLiteral("RenamedActionTarget"), wavPath, asset.sourceID);
    ASSERT_NE(target, nullptr);
    auto* action = projects.createActionTargeting(
        target, VNX::VActionStep::Type::Play, originalName);
    ASSERT_NE(action, nullptr);
    const harkenstudio::ObjectId actionStableId = action->runtimeId();
    ASSERT_NE(actionStableId, harkentest::StudioObjectId(0u));
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    const auto pendingRevision = engine.runtimeSyncCoordinator()
        .markCurrentWorldMutation();
    ASSERT_EQ(pendingRevision.state,
              VNX::RuntimeSyncCoordinator::AllocationState::Allocated);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    QStringList dispatchedNames;
    QObject eventObserver;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts](uint32_t, const QString&, int32_t) {
            ++failedStarts;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });
    QObject::connect(
        &engine, &VNX::AudioEngine::playbackStarted, &eventObserver,
        [&dispatchedNames](quintptr handle, const QString& name) {
            if (handle != 0)
                dispatchedNames.push_back(name);
        });
    const uint32_t playingId = audition.playItem(*action);
    ASSERT_NE(playingId, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    EXPECT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_EQ(successfulStarts, 1);
    EXPECT_EQ(failedStarts, 0);
    EXPECT_EQ(dispatchedNames, QStringList{originalName});
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
}

TEST(AudioEnginePreview,
     SequenceReorderPublishesOneRevisionAndStartsFirstPreviewIntent)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("sequence.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("ImmediateSequenceReorderPreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));

    VNX::AssetInfo asset;
    asset.sourceID = 6993;
    asset.relativePath = QStringLiteral("sequence.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* container = projects.createItem(
        layer, QStringLiteral("Sequence"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    auto* first = projects.createAudioItem(
        layer, QStringLiteral("First"), wavPath, asset.sourceID);
    auto* second = projects.createAudioItem(
        layer, QStringLiteral("Second"), wavPath, asset.sourceID);
    auto* third = projects.createAudioItem(
        layer, QStringLiteral("Third"), wavPath, asset.sourceID);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    ASSERT_TRUE(projects.moveItem(first, container));
    ASSERT_TRUE(projects.moveItem(second, container));
    ASSERT_TRUE(projects.moveItem(third, container));
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = VNX::SelectionMode::Sequence;

    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto revisionBeforeReorder =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();

    ContainerActionReset actions(projects);
    QJsonArray order;
    order.append(third->name);
    order.append(second->name);
    order.append(first->name);
    const auto reordered = VNX::ActionManager::instance().execute(
        QStringLiteral("reorder_container_children"),
        QJsonObject{
            {QStringLiteral("container_id"),
             static_cast<qint64>(container->objectID)},
            {QStringLiteral("child_order"), order}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(reordered.success) << reordered.errorMessage.toStdString();

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts](uint32_t, const QString&, int32_t) {
            ++failedStarts;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });
    const uint32_t playingId = audition.playItem(*container);
    ASSERT_NE(playingId, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    EXPECT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_EQ(successfulStarts, 1);
    EXPECT_TRUE(engine.hasInstalledCurrentWorld());
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeReorder + 1);
    EXPECT_EQ(failedStarts, 0);
    audition.stopAll(0);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(failedStarts, 0);
}

TEST(AudioEnginePreview,
     RandomToSequenceStartsFirstPreviewIntentWithoutReselection)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("mode-switch.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("ImmediateModeSwitchPreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));

    VNX::AssetInfo asset;
    asset.sourceID = 6994;
    asset.relativePath = QStringLiteral("mode-switch.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* container = projects.createItem(
        layer, QStringLiteral("Container"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    for (int index = 0; index < 3; ++index) {
        auto* sound = projects.createAudioItem(
            container, QStringLiteral("Sound%1").arg(index + 1),
            wavPath, asset.sourceID);
        ASSERT_NE(sound, nullptr);
    }
    const auto* initialSelection = container->comp<VNX::VSelection>();
    ASSERT_NE(initialSelection, nullptr);
    ASSERT_EQ(initialSelection->mode, VNX::SelectionMode::Random);

    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto revisionBeforeModeChange =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();

    ContainerActionReset actions(projects);
    const auto changed = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Sequence)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(changed.success) << changed.errorMessage.toStdString();

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    int32_t failureResult = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts, &failureResult](uint32_t, const QString&, int32_t result) {
            ++failedStarts;
            failureResult = result;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });
    const uint32_t playingId = audition.playItem(*container);
    ASSERT_NE(playingId, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    EXPECT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_EQ(successfulStarts, 1);
    EXPECT_TRUE(engine.hasInstalledCurrentWorld());
    EXPECT_EQ(failedStarts, 0) << "engine result " << failureResult;
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeModeChange + 1);
    audition.stopAll(0);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
}

TEST(AudioEnginePreview,
     VariableControlDiscreteEditPublishesOneRevisionAndStartsFirstPreviewIntent)
{
    expectVariableControlGridMutationFirstIntent(
        QStringLiteral("VariableControlDiscretePreview"), 6995,
        discreteGridJson);
}

TEST(AudioEnginePreview,
     VariableControlContinuousEditPublishesOneRevisionAndStartsFirstPreviewIntent)
{
    expectVariableControlGridMutationFirstIntent(
        QStringLiteral("VariableControlContinuousPreview"), 6996,
        blendsJson, VNX::SelectionMode::Blend);
}

TEST(AudioEnginePreview,
     DefaultBlendEditPublishesOneRevisionAndStartsFirstPreviewIntent)
{
    expectVariableControlGridMutationFirstIntent(
        QStringLiteral("DefaultBlendPreview"), 6998,
        defaultBlendJson, VNX::SelectionMode::Blend);
}

TEST(AudioEnginePreview,
     NestedMatchBlendEditPublishesOneRevisionAndStartsFirstPreviewIntent)
{
    expectVariableControlGridMutationFirstIntent(
        QStringLiteral("NestedMatchBlendPreview"), 6997,
        nestedMatchGridJson, VNX::SelectionMode::Match, true);
}

TEST(AudioEnginePreview,
     VariableControlRehearsalBeforePlaySelectsAudibleTargetOnFirstIntent)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlRehearsalFirstIntent");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 6999);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    const auto revisionBeforeRehearsal =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeRehearsal);

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    for (int attempt = 0;
         attempt < 32
         && audition.getAudition(playingId).playbackHandle == 0; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        runtime->nullBackend.Pump(1);
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "retryPendingPreviewStarts", Qt::DirectConnection));
    }
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);

    const float peak = pumpUntilPreviewAudible();
    EXPECT_GT(peak, 0.4f)
        << "first Preview intent rendered the default 0.125 PCM instead of "
           "the rehearsal-selected 0.75 PCM";
}

class ActionRehearsalScopeTest : public testing::TestWithParam<VNX::VariableScope> {};

TEST_P(ActionRehearsalScopeTest, WaitsForLatestValueAndCancelsPendingIntent)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlRehearsalFirstIntent");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 6999);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* action = projects.createActionTargeting(fixture.container, VNX::VActionStep::Type::Play,
                                                 QStringLiteral("Play_Rehearsal"));
    ASSERT_NE(action, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto& rehearsal = projects.variableRehearsal();
    rehearsal.setScope(fixture.step->guid, GetParam());
    HarkenEditor::AuditionManager audition(&engine);
    int starts = 0;
    QObject::connect(&audition, &HarkenEditor::AuditionManager::auditionStarted,
                     &audition, [&starts](uint32_t, const QString&) { ++starts; });
    const auto serviceAndRetry = [&] {
        EXPECT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        harken::detail::ActiveEngine()->nullBackend.Pump(1);
        EXPECT_TRUE(QMetaObject::invokeMethod(
            &audition, "retryPendingPreviewStarts", Qt::DirectConnection));
    };
    const auto awaitStart = [&](uint32_t playingId) {
        for (int attempt = 0; attempt < 16
             && audition.getAudition(playingId).playbackHandle == 0; ++attempt)
            serviceAndRetry();
        EXPECT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    };
    const auto stopAndDrain = [&](uint32_t playingId) {
        audition.stop(playingId);
        for (int block = 0; block < 32; ++block)
            serviceAndRetry();
    };

    // The first intent must wait for the new emitter and the latest override.
    ASSERT_TRUE(rehearsal.setOverride(fixture.step->guid, 1.0f));
    const auto first = audition.playItem(*action);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(audition.getAudition(first).playbackHandle, 0u);
    EXPECT_TRUE(audition.getAudition(first).previewStartPending);
    EXPECT_EQ(starts, 0);
    ASSERT_TRUE(rehearsal.setOverride(fixture.step->guid, 2.0f));
    awaitStart(first);
    EXPECT_GT(pumpUntilPreviewAudible(), 0.4f);
    EXPECT_EQ(starts, 1);
    stopAndDrain(first);

    // An existing emitter must also wait after an immediate variable edit.
    ASSERT_TRUE(rehearsal.setOverride(fixture.step->guid, 1.0f));
    const auto second = audition.playItem(*action);
    ASSERT_NE(second, 0u);
    EXPECT_EQ(audition.getAudition(second).playbackHandle, 0u);
    awaitStart(second);
    const float secondPeak = pumpUntilPreviewAudible();
    EXPECT_GT(secondPeak, 0.05f);
    EXPECT_LT(secondPeak, 0.4f);
    EXPECT_EQ(starts, 2);
    stopAndDrain(second);

    // Cancelling the pending intent must prevent a delayed Action dispatch.
    ASSERT_TRUE(rehearsal.setOverride(fixture.step->guid, 2.0f));
    const auto cancelled = audition.playItem(*action);
    ASSERT_NE(cancelled, 0u);
    EXPECT_EQ(audition.getAudition(cancelled).playbackHandle, 0u);
    stopAndDrain(cancelled);
    EXPECT_EQ(audition.activeAuditionCount(), 0);
    EXPECT_EQ(starts, 2);
}

INSTANTIATE_TEST_SUITE_P(
    EmitterAndGlobal, ActionRehearsalScopeTest,
    testing::Values(VNX::VariableScope::Emitter, VNX::VariableScope::Global));

TEST(AudioEnginePreview,
     ActionBridgeUsesAppliedEmitterVariable)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlRehearsalFirstIntent");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 6999);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* action = projects.createActionTargeting(fixture.container, VNX::VActionStep::Type::Play,
                                                 QStringLiteral("Play_Rehearsal"));
    ASSERT_NE(action, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(Vn_CreateEmitter(&emitter), VN_OK);
    HarkenEditor::Bridge::VariableTicket ticket;
    ASSERT_EQ(HarkenEditor::Bridge::writePreviewVariable(
        fixture.step->runtimeId(), 2.0f, emitter, ticket), HarkenEditor::Bridge::kEngineOk);
    VnVariableTicketQueryDescV1 query{};
    query.structSize = sizeof(query);
    query.ticket = {ticket.sequence, ticket.worldGeneration, ticket.transportGeneration};
    VnVariableTicketQueryResultV1 result{};
    result.structSize = sizeof(result);
    for (int attempt = 0; attempt < 256; ++attempt) {
        ASSERT_EQ(Vn_QueryVariableTicket(&query, &result), VN_OK);
        ASSERT_EQ(result.status, VN_VARIABLE_TICKET_FOUND);
        if (result.outcome != VN_VARIABLE_OUTCOME_PENDING) break;
        ASSERT_EQ(Vn_Update(), VN_OK);
        harken::detail::ActiveEngine()->nullBackend.Pump(1);
    }
    ASSERT_EQ(result.outcome, VN_VARIABLE_OUTCOME_APPLIED);
    HarkenEditor::Bridge::EngineResult posted;
    const auto handle = HarkenEditor::Bridge::Detail::runtimePlayByName(
        action->name, false, 0, 0, 0, &posted, emitter);
    ASSERT_EQ(posted, HarkenEditor::Bridge::kEngineOk);
    ASSERT_NE(handle, 0u);
    EXPECT_GT(pumpUntilPreviewAudible(), 0.4f);
    EXPECT_TRUE(HarkenEditor::Bridge::Detail::runtimeStopPlayback(handle, 0));
    EXPECT_TRUE(harken::detail::ActiveEngine()->emitters.IsLive(emitter));
    EXPECT_EQ(Vn_DestroyEmitter(emitter), VN_OK);
}

TEST(AudioEnginePreview,
     VariableControlRehearsalRetargetsAudibleMemberWhilePreviewIsPlaying)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlRehearsalRetarget");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7000);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);
    auto* selection = fixture.container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    VNX::TransitionRuleDef transitionRule =
        selection->transitions.rules[0];
    transitionRule.exitTiming = VNX::TransitionExitTiming::Immediate;
    transitionRule.sourceFadeOut.durationSeconds = 0.004f;
    transitionRule.targetFadeIn.durationSeconds = 0.004f;
    ASSERT_TRUE(selection->transitions.updateRule(0, transitionRule));

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"),
             gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto revisionBeforeResponse =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();
    const auto responseChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("variableResponse")},
            {QStringLiteral("value"), static_cast<int>(VNX::VariableResponse::Live)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(responseChanged.success) << responseChanged.errorMessage.toStdString();
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeResponse + 1);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(selection->variableResponse(), VNX::VariableResponse::Live);

    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(selection->variableResponse(), VNX::VariableResponse::AtStart);
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeResponse + 2);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(harken::detail::ActiveEngine()->variables.CurrentPlayableWorld().transitionCount, 0u);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_EQ(selection->variableResponse(), VNX::VariableResponse::Live);
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeResponse + 3);

    const bool worldReady = serviceUntilCurrentWorldReady(engine);
    const auto worldBarrier = engine.runtimeSyncCoordinator().barrier(
        engine.runtimeSyncCoordinator().currentAuthoringRevision());
    ASSERT_TRUE(worldReady)
        << "barrierState=" << static_cast<int>(worldBarrier.state)
        << " failure=" << static_cast<int>(worldBarrier.failure)
        << " requiredRevision=" << worldBarrier.requiredRevision
        << " installedRevision=" << worldBarrier.installedRevision;
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    const harken::PlayableWorldViewV1 currentWorld =
        runtime->variables.CurrentPlayableWorld();
    ASSERT_EQ(currentWorld.transitionCount, 1u);
    ASSERT_NE(currentWorld.transitions, nullptr);
    ASSERT_EQ(currentWorld.transitions[0].ruleCount, 1u);
    EXPECT_GT(currentWorld.transitions[0].rules[0]
                  .sourceFade.durationFrames,
              0u);
    EXPECT_GT(currentWorld.transitions[0].rules[0]
                  .targetFade.durationFrames,
              0u);

    VNX::BlueprintCanvasWidget canvas;
    ASSERT_TRUE(canvas.focusItem(fixture.container));
    ASSERT_TRUE(canvas.activeSignalPathSource()->graph().ok());
    const auto activeSound = [&](const VNX::ProjectItem* sound) {
        const auto& nodes = canvas.activeSignalPathSource()->graph().nodes;
        for (size_t index = 0; index < nodes.size(); ++index)
            if (nodes[index].key.objectGuid == sound->guid)
                return canvas.signalPathNode(index)->playbackActive();
        return false;
    };

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    const quintptr previewHandle =
        audition.getAudition(playingId).playbackHandle;
    const float initialPeak = pumpUntilPreviewAudible();
    ASSERT_GT(initialPeak, 0.1f);
    ASSERT_LT(initialPeak, 0.2f);

    QMetaObject::invokeMethod(&engine, "onMonitorTimer", Qt::DirectConnection);
    EXPECT_TRUE(activeSound(fixture.first));
    EXPECT_FALSE(activeSound(fixture.second));

    const auto revisionBeforeRehearsal =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeRehearsal);

    QMetaObject::invokeMethod(&engine, "onMonitorTimer", Qt::DirectConnection);
    EXPECT_TRUE(activeSound(fixture.first));
    EXPECT_FALSE(activeSound(fixture.second));

    const float retargetedPeak = pumpUntilPreviewPeakAbove(0.4f);
    EXPECT_GT(retargetedPeak, 0.4f)
        << "the live Variable Control preview did not retarget from the "
           "0.125 PCM member to the 0.75 PCM member";
    EXPECT_EQ(audition.activeAuditionCount(), 1);
    EXPECT_EQ(audition.getAudition(playingId).playbackHandle,
              previewHandle);
    VNX::AudioEngine::PreviewPlayableState state{};
    ASSERT_EQ(engine.queryPreviewPlayable(previewHandle, state), kPreviewOk);
    EXPECT_NE(state.lastAppliedBlockSequence, 0u);
    EXPECT_NE(state.childLiveCount, 0u);

    QMetaObject::invokeMethod(&engine, "onMonitorTimer", Qt::DirectConnection);
    EXPECT_TRUE(activeSound(fixture.second));
    for (int block = 0; block < 32 && activeSound(fixture.first); ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        runtime->nullBackend.Pump(1);
        QMetaObject::invokeMethod(&engine, "onMonitorTimer", Qt::DirectConnection);
    }
    EXPECT_FALSE(activeSound(fixture.first));
    EXPECT_TRUE(activeSound(fixture.second));
}

TEST(AudioEnginePreview,
     DiscreteVariableControlRetargetsFirstPreviewFromSoundToCompositeTrack)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlCompositeTrack");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7010);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.first, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    const QString audioSourcesPath = QDir(
        QFileInfo(projects.currentProject()->projectPath).absolutePath())
            .filePath(QStringLiteral("AudioSources"));
    auto* track = projects.createItem(
        fixture.container, QStringLiteral("Composite Track"),
        VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    const std::array<float, 2> levels{0.25f, 0.5f};
    for (std::uint32_t index = 0; index < levels.size(); ++index) {
        const QString fileName =
            QStringLiteral("composite-track-%1.wav").arg(index + 1);
        const QString wavPath = QDir(audioSourcesPath).filePath(fileName);
        writeWavF32(wavPath, std::vector<float>(65536, levels[index]));
        VNX::AssetInfo asset;
        asset.sourceID = 7110 + index;
        asset.relativePath = fileName;
        asset.absolutePath = wavPath;
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
        auto* sound = projects.createAudioItem(
            track, QStringLiteral("Track Sound %1").arg(index + 1),
            wavPath, asset.sourceID);
        ASSERT_NE(sound, nullptr);
        ASSERT_EQ(timeline->clips.items.size(), index + 1);
        EXPECT_EQ(timeline->clips.items.back().targetGuid, sound->guid);
    }

    auto* selection = fixture.container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = VNX::SelectionMode::Match;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(fixture.step->guid));
        ASSERT_TRUE(grid.addRow(fixture.first->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 1));
        ASSERT_TRUE(grid.addRow(track->guid));
        ASSERT_TRUE(grid.setCell(1, 0, 2));
        EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    selection->matchResponse =
        VNX::VariableResponse::Live;
    VNX::TransitionRuleDef transitionRule =
        selection->transitions.rules[0];
    transitionRule.exitTiming = VNX::TransitionExitTiming::Immediate;
    transitionRule.sourceFadeOut.durationSeconds = 0.004f;
    transitionRule.targetFadeIn.durationSeconds = 0.004f;
    ASSERT_TRUE(selection->transitions.updateRule(0, transitionRule));

    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->variables.CurrentPlayableWorld().transitionCount, 1u);

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    const quintptr previewHandle =
        audition.getAudition(playingId).playbackHandle;
    ASSERT_NE(previewHandle, 0u);
    harken::VoiceReleaseObserver originalSource{};
    for (const auto& row : runtime->playableServices) {
        if (runtime->voices.Resolve(row.renderHandle) != nullptr) {
            ASSERT_TRUE(runtime->voices.CaptureReleaseObserver(
                row.renderHandle, &originalSource));
            break;
        }
    }
    ASSERT_TRUE(originalSource.Valid());
    const float initialPeak = pumpUntilPreviewAudible();
    ASSERT_GT(initialPeak, 0.1f);
    ASSERT_LT(initialPeak, 0.2f);
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));
    const float retargetedPeak = pumpUntilPreviewPeakAbove(0.7f);
    EXPECT_GT(retargetedPeak, 0.7f);
    EXPECT_EQ(audition.getAudition(playingId).playbackHandle,
              previewHandle);
    for (uint32_t block = 0; block < 32
         && !runtime->voices.ReleaseObserved(originalSource); ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        runtime->nullBackend.Pump(1);
    }
    ASSERT_TRUE(runtime->voices.ReleaseObserved(originalSource));
    ASSERT_EQ(Vn_Update(), VN_OK);
    VNX::AudioEngine::PreviewPlayableState state{};
    ASSERT_EQ(engine.queryPreviewPlayable(previewHandle, state), kPreviewOk);
    EXPECT_EQ(state.childLiveCount, 2u);
    EXPECT_EQ(state.childAudibleRetiringCount, 0u);
}

TEST(AudioEnginePreview,
     ClearingVariableControlRehearsalReturnsLivePreviewToGlobalValue)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlRehearsalClear");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7001);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"),
             gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), continuouslyEvaluatedDiscreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* currentRuntime = harken::detail::ActiveEngine();
    ASSERT_NE(currentRuntime, nullptr);
    EXPECT_EQ(currentRuntime->variables.CurrentPlayableWorld().transitionCount, 0u);

    const auto revisionBeforeRehearsal =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));
    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    for (int attempt = 0;
         attempt < 32
         && audition.getAudition(playingId).playbackHandle == 0; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        runtime->nullBackend.Pump(1);
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "retryPendingPreviewStarts", Qt::DirectConnection));
    }
    const quintptr previewHandle =
        audition.getAudition(playingId).playbackHandle;
    ASSERT_NE(previewHandle, 0u);
    ASSERT_GT(pumpUntilPreviewPeakAbove(0.4f), 0.4f);

    projects.variableRehearsal().setScope(fixture.step->guid, VNX::VariableScope::Global);
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, fixture.step->comp<VNX::VVariable>()->defaultValue));
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeRehearsal);
    const float globalPeak = pumpUntilPreviewPeakInRange(0.1f, 0.2f);
    EXPECT_GT(globalPeak, 0.1f)
        << "global default write did not select the 0.125 PCM member";
    EXPECT_LT(globalPeak, 0.2f);
    EXPECT_EQ(audition.getAudition(playingId).playbackHandle,
              previewHandle);
}

TEST(AudioEnginePreview,
     LatestVariableControlRehearsalBeforeReadyDrivesTheSinglePreviewIntent)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlRehearsalBeforeReady");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7002);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();

    const auto revisionBeforeRehearsal =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 1.0f));
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeRehearsal);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    for (int attempt = 0;
         attempt < 64
         && audition.getAudition(playingId).playbackHandle == 0; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        runtime->nullBackend.Pump(1);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "retryPendingPreviewStarts", Qt::DirectConnection));
    }
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_GT(pumpUntilPreviewPeakAbove(0.4f), 0.4f)
        << "the Preview intent used an obsolete pre-Ready rehearsal value "
           "instead of the latest 2.0 value";
}

TEST(AudioEnginePreview,
     VariableControlRehearsalReplaysToTheNewPreviewEmitterAfterRuntimeRestart)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlRehearsalRestart");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7003);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));
    const auto expectSelectedMemberOnOneIntent = [&]() {
        HarkenEditor::AuditionManager audition(&engine);
        const uint32_t playingId = audition.playItem(*fixture.container);
        ASSERT_NE(playingId, 0u);
        for (int attempt = 0;
             attempt < 64
             && audition.getAudition(playingId).playbackHandle == 0; ++attempt) {
            ASSERT_TRUE(QMetaObject::invokeMethod(
                &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
            auto* runtime = harken::detail::ActiveEngine();
            ASSERT_NE(runtime, nullptr);
            runtime->nullBackend.Pump(1);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            ASSERT_TRUE(QMetaObject::invokeMethod(
                &audition, "retryPendingPreviewStarts",
                Qt::DirectConnection));
        }
        ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);
        EXPECT_GT(pumpUntilPreviewPeakAbove(0.4f), 0.4f);
        audition.stopAll();
    };

    expectSelectedMemberOnOneIntent();
    const auto epochBeforeRestart =
        engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch;
    engine.stopOutput();
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_GT(engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch,
              epochBeforeRestart);
    expectSelectedMemberOnOneIntent();
}

TEST(AudioEnginePreview,
     UnacceptedVariableRehearsalDoesNotBlockInstalledWorldPlay)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlUnacceptedRehearsal");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7004);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_TRUE(engine.hasInstalledCurrentWorld());

    HarkenEditor::AuditionManager initialAudition(&engine);
    const uint32_t initialPlayingId =
        initialAudition.playItem(*fixture.container);
    ASSERT_NE(initialPlayingId, 0u);
    ASSERT_NE(initialAudition.getAudition(initialPlayingId).playbackHandle, 0u);
    ASSERT_GT(pumpUntilPreviewAudible(), 0.1f);

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    auto auditionReset = harken::BeginRuntimeTransportReset(*runtime, 0);
    ASSERT_EQ(auditionReset,
              harken::RuntimeTransportResetStatus::Backpressure);
    const std::uint64_t acceptedBefore = runtime->variables.AcceptedCount();
    const auto revisionBeforeRehearsal =
        engine.runtimeSyncCoordinator().currentAuthoringRevision();
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));
    EXPECT_EQ(runtime->variables.AcceptedCount(), acceptedBefore);
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(),
              revisionBeforeRehearsal);

    for (uint32_t block = 0;
         block < 128
         && auditionReset
             == harken::RuntimeTransportResetStatus::Backpressure;
         ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        runtime->nullBackend.Pump(1);
        ASSERT_EQ(Vn_Update(), VN_OK);
        auditionReset = harken::BeginRuntimeTransportReset(*runtime, 0);
    }
    ASSERT_EQ(auditionReset,
              harken::RuntimeTransportResetStatus::Complete);

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    EXPECT_NE(audition.lastPlayReason(), QStringLiteral("rehearsal"));
    for (int attempt = 0;
         attempt < 16
         && audition.getAudition(playingId).playbackHandle == 0; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        runtime->nullBackend.Pump(1);
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "retryPendingPreviewStarts", Qt::DirectConnection));
    }
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    EXPECT_FALSE(audition.getAudition(playingId).previewStartPending);
    EXPECT_TRUE(audition.getAudition(playingId).bIsPlaying);
    EXPECT_GT(pumpUntilPreviewAudible(), 0.05f);
}

TEST(AudioEnginePreview,
     VariableCapacityRefusalKeepsLatestRehearsalAndPreviewIntentPending)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableCapacityPreviewRetry");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7005);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    const harkenstudio::ObjectId variableId = fixture.step->runtimeId();
    ASSERT_NE(variableId, harkentest::StudioObjectId(0u));
    const auto observation = harken::ObserveVariableCallAt(
        runtime->variables.CurrentWorldGeneration(),
        runtime->variables.CurrentTransportEpoch(), runtime->config.blockFrames,
        runtime->timelinePool.WindowStartFrame(),
        runtime->timelinePool.PublishCutoffFrame());
    std::vector<harken::PreparedVariableWriteToken> pendingWrites(
        runtime->variables.PendingCapacity());
    for (auto& write : pendingWrites) {
        ASSERT_EQ(VN_OK, runtime->variables.PrepareGlobalWrite(
            harkentest::ToRuntime(variableId), 1.0f, observation, 0, &write).result);
    }
    ASSERT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED,
              Vn_SetValue(harkentest::ToRuntime(variableId), 1.0f));

    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));
    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    EXPECT_NE(audition.lastPlayReason(), QStringLiteral("rehearsal"));
    EXPECT_NE(audition.lastPlayReason(), QStringLiteral("occupancy"));
    for (auto& write : pendingWrites)
        runtime->variables.CancelPreparedWrite(&write);
    for (int attempt = 0;
         attempt < 16
         && audition.getAudition(playingId).playbackHandle == 0; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        runtime->nullBackend.Pump(1);
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "retryPendingPreviewStarts", Qt::DirectConnection));
    }
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_EQ(audition.activeAuditionCount(), 1);
}

TEST(AudioEnginePreview,
     NestedMatchBlendReloadAndRestartsKeepFirstPreviewIntent)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("VariableControlLifecyclePreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(
        projects, 6998, 65536, true);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.blend, nullptr);
    const QUuid containerGuid = fixture.container->guid;
    const QUuid blendGuid = fixture.blend->guid;

    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), nestedMatchGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success)
        << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_TRUE(projects.saveProject());
    const QString projectPath = projects.currentProject()->projectPath;
    ASSERT_TRUE(projects.loadProject(projectPath));
    for (int attempt = 0;
         attempt < 32 && projects.currentProject() == nullptr; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    ASSERT_NE(projects.currentProject(), nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* reloaded = projects.findItemByGuid(containerGuid);
    ASSERT_NE(reloaded, nullptr);
    const auto* selection = reloaded->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    EXPECT_EQ(selection->mode, VNX::SelectionMode::Match);
    EXPECT_FALSE(selection->matchGrid().empty());
    EXPECT_TRUE(selection->mixing().empty());
    const auto* reloadedBlend = projects.findItemByGuid(blendGuid);
    ASSERT_NE(reloadedBlend, nullptr);
    EXPECT_EQ(reloadedBlend->parent, reloaded);
    const auto* blendSelection = reloadedBlend->comp<VNX::VSelection>();
    ASSERT_NE(blendSelection, nullptr);
    EXPECT_EQ(blendSelection->mode, VNX::SelectionMode::Blend);
    EXPECT_EQ(blendSelection->variableResponse(), VNX::VariableResponse::Live);
    EXPECT_TRUE(blendSelection->matchGrid().empty());
    EXPECT_FALSE(blendSelection->mixing().empty());

    const auto expectFirstIntent = [&engine](VNX::ProjectItem& container) {
        HarkenEditor::AuditionManager audition(&engine);
        int failedStarts = 0;
        int successfulStarts = 0;
        QObject::connect(
            &audition,
            &HarkenEditor::AuditionManager::auditionStartFailed,
            [&failedStarts](uint32_t, const QString&, int32_t) {
                ++failedStarts;
            });
        QObject::connect(
            &audition, &HarkenEditor::AuditionManager::auditionStarted,
            [&successfulStarts](uint32_t, const QString&) {
                ++successfulStarts;
            });
        const uint32_t playingId = audition.playItem(container);
        ASSERT_NE(playingId, 0u);
        for (int attempt = 0; attempt < 32 && successfulStarts == 0
             && failedStarts == 0; ++attempt) {
            ASSERT_TRUE(QMetaObject::invokeMethod(
                &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
        EXPECT_EQ(successfulStarts, 1);
        EXPECT_EQ(failedStarts, 0);
        ASSERT_EQ(audition.activeAuditionCount(), 1);
        EXPECT_NE(audition.getAudition(playingId).playbackHandle, 0u);
        audition.stopAll();
    };

    expectFirstIntent(*reloaded);
    const auto epochBeforeRestart =
        engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch;
    engine.stopOutput();
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_GT(engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch,
              epochBeforeRestart);
    expectFirstIntent(*reloaded);

    const auto epochBeforeReconfigure =
        engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch;
    ASSERT_TRUE(engine.reconfigureBufferSize(128));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_GT(engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch,
              epochBeforeReconfigure);
    expectFirstIntent(*reloaded);
}

TEST(AudioEnginePreview, PreviewRevisionWaitTimesOutWithoutStartingStaleWorld)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("timeout.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("RevisionTimeoutPreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6992;
    asset.relativePath = QStringLiteral("timeout.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = projects.createAudioItem(
        layer, QStringLiteral("TimeoutSound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    const auto mutation = engine.runtimeSyncCoordinator()
        .markCurrentWorldMutation();
    ASSERT_EQ(mutation.state,
              VNX::RuntimeSyncCoordinator::AllocationState::Allocated);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts](uint32_t, const QString&, int32_t) {
            ++failedStarts;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });

    const uint32_t playingId = audition.playItem(*sound);
    ASSERT_NE(playingId, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).revisionPending);
    EXPECT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_EQ(1, successfulStarts);
    EXPECT_EQ(0, failedStarts);
    EXPECT_EQ(1, audition.activeAuditionCount());
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
}

TEST(AudioEnginePreview, SpacebarStopsInstalledWorldWhilePlaying)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("restart.wav"));
    writeWavF32(wavPath, std::vector<float>(4096, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("SpacebarStopPreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6995;
    asset.relativePath = QStringLiteral("restart.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = projects.createAudioItem(
        layer, QStringLiteral("RestartSound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_TRUE(engine.hasInstalledCurrentWorld());

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t firstId = audition.playItem(*sound);
    ASSERT_NE(firstId, 0u);
    const quintptr firstHandle = audition.getAudition(firstId).playbackHandle;
    ASSERT_NE(firstHandle, 0u);
    EXPECT_FALSE(audition.getAudition(firstId).revisionPending);
    EXPECT_GT(pumpUntilPreviewAudible(), 0.05f);

    audition.stopAll(100);
    EXPECT_FALSE(audition.getAudition(firstId).bIsPlaying);
    bool stopped = !audition.hasActiveNonRawAudition();
    for (int attempt = 0; attempt < 64 && !stopped; ++attempt)
        pumpPreviewTransport(engine, audition);
    EXPECT_FALSE(audition.hasActiveNonRawAudition());
    EXPECT_EQ(audition.getAudition(firstId).playingId, 0u);
}

TEST(AudioEnginePreview, SecondPlayItemKeepsPreviousPreviewAlive)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("stack.wav"));
    writeWavF32(wavPath, std::vector<float>(4096, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("StackedPreviewInstances");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6996;
    asset.relativePath = QStringLiteral("stack.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = projects.createAudioItem(
        layer, QStringLiteral("StackSound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t firstId = audition.playItem(*sound);
    ASSERT_NE(firstId, 0u);
    const quintptr firstHandle = audition.getAudition(firstId).playbackHandle;
    ASSERT_NE(firstHandle, 0u);
    EXPECT_GT(pumpUntilPreviewAudible(), 0.05f);

    const uint32_t secondId = audition.playItem(*sound);
    ASSERT_NE(secondId, 0u);
    EXPECT_NE(secondId, firstId);
    const auto firstAfter = audition.getAudition(firstId);
    const auto second = audition.getAudition(secondId);
    EXPECT_EQ(firstAfter.playbackHandle, firstHandle);
    EXPECT_NE(second.playbackHandle, 0u);
    EXPECT_NE(second.playbackHandle, firstHandle);
    EXPECT_TRUE(firstAfter.bIsPlaying);
    EXPECT_TRUE(second.bIsPlaying);
    EXPECT_EQ(firstAfter.playingId, firstId);
    EXPECT_EQ(second.playingId, secondId);
    EXPECT_GT(pumpUntilPreviewAudible(), 0.05f);
}

float queryVariableCurrent(harkenstudio::ObjectId variableId)
{
    VnVariableQueryDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = harkentest::ToRuntime(variableId);
    VnVariableQueryResultV1 result{};
    result.structSize = sizeof(result);
    if (Vn_QueryVariable(&desc, &result) != VN_OK)
        return std::numeric_limits<float>::quiet_NaN();
    return result.currentValue;
}

TEST(AudioEnginePreview, RapidRehearsalOverridesResolveToLatestAppliedValue)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("RapidRehearsalWrites");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7601);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.smooth, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    ASSERT_GT(pumpUntilPreviewAudible(), 0.05f);

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    const harkenstudio::ObjectId variableId = fixture.smooth->runtimeId();
    ASSERT_FALSE(harkenstudio::IsNull(variableId));
    const std::uint64_t acceptedBefore = runtime->variables.AcceptedCount();
    const std::uint64_t appliedBefore = runtime->variables.AppliedCount();
    projects.variableRehearsal().setScope(fixture.smooth->guid, VNX::VariableScope::Global);
    constexpr int kFlips = 16;
    float lastIntent = 0.0f;
    for (int flip = 0; flip < kFlips; ++flip) {
        lastIntent = (flip % 2) == 0 ? 0.0f : 1.0f;
        ASSERT_TRUE(projects.variableRehearsal().setOverride(
            fixture.smooth->guid, lastIntent));
    }
    const std::uint64_t acceptedAfterBurst =
        runtime->variables.AcceptedCount();
    const std::uint64_t acceptedDelta = acceptedAfterBurst - acceptedBefore;
    EXPECT_EQ(acceptedDelta, static_cast<std::uint64_t>(kFlips));

    std::vector<float> appliedAfterRelease;
    std::uint64_t appliedSeen = appliedBefore;
    int idlePumps = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        ASSERT_EQ(Vn_Update(), VN_OK);
        runtime->nullBackend.Pump(1);
        const std::uint64_t appliedNow = runtime->variables.AppliedCount();
        if (appliedNow > appliedSeen) {
            appliedAfterRelease.push_back(queryVariableCurrent(variableId));
            appliedSeen = appliedNow;
            idlePumps = 0;
        } else {
            ++idlePumps;
        }
        VnVariableQueryDescV1 desc{};
        desc.structSize = sizeof(desc);
        desc.variableId = harkentest::ToRuntime(variableId);
        VnVariableQueryResultV1 query{};
        query.structSize = sizeof(query);
        ASSERT_EQ(Vn_QueryVariable(&desc, &query), VN_OK);
        if (query.pendingCount == 0 && idlePumps >= 2)
            break;
    }
    EXPECT_LE(appliedAfterRelease.size(), 2u);
    for (float applied : appliedAfterRelease) {
        EXPECT_TRUE(applied == 0.0f || applied == lastIntent)
            << "intermediate Applied value " << applied;
    }
    EXPECT_FLOAT_EQ(queryVariableCurrent(variableId), lastIntent);
    EXPECT_EQ(runtime->variables.AcceptedCount() - acceptedBefore,
              static_cast<std::uint64_t>(kFlips));
}

TEST(AudioEnginePreview, PendingWorldStillAcceptsLatestRehearsalWrite)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("PendingWorldLiveWrite");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7602);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.smooth, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    const auto mutation = engine.runtimeSyncCoordinator()
        .markCurrentWorldMutation();
    ASSERT_EQ(mutation.state,
              VNX::RuntimeSyncCoordinator::AllocationState::Allocated);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    EXPECT_EQ(engine.runtimeSyncCoordinator().barrier(mutation.revision).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Pending);
    ASSERT_TRUE(engine.hasInstalledCurrentWorld());

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    const std::uint64_t acceptedBefore = runtime->variables.AcceptedCount();
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.smooth->guid, 1.0f));
    EXPECT_GT(runtime->variables.AcceptedCount(), acceptedBefore);
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
}

TEST(AudioEnginePreview, InstalledWorldResubmitsCurrentRehearsalWithoutAnotherDrag)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("InstalledWorldResubmitRehearsal");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7603);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.smooth, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*fixture.container);
    ASSERT_NE(playingId, 0u);
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    const harkenstudio::ObjectId variableId = fixture.smooth->runtimeId();
    ASSERT_FALSE(harkenstudio::IsNull(variableId));
    projects.variableRehearsal().setScope(fixture.smooth->guid, VNX::VariableScope::Global);
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.smooth->guid, 1.0f));
    bool firstApplied = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        ASSERT_EQ(Vn_Update(), VN_OK);
        runtime->nullBackend.Pump(1);
        if (queryVariableCurrent(variableId) == 1.0f) {
            firstApplied = true;
            break;
        }
    }
    ASSERT_TRUE(firstApplied);
    const std::uint64_t acceptedAfterFirst =
        runtime->variables.AcceptedCount();

    audition.stopAll(0);
    for (int attempt = 0; attempt < 8; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        ASSERT_EQ(Vn_Update(), VN_OK);
        runtime->nullBackend.Pump(1);
    }

    auto* smooth = fixture.smooth->comp<VNX::VVariable>();
    ASSERT_NE(smooth, nullptr);
    smooth->defaultValue = 0.25f;
    const auto mutation = engine.runtimeSyncCoordinator()
        .markCurrentWorldMutation();
    ASSERT_EQ(mutation.state,
              VNX::RuntimeSyncCoordinator::AllocationState::Allocated);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    if (!serviceUntilCurrentWorldReady(engine)) {
        const auto barrier = engine.runtimeSyncCoordinator().barrier(
            engine.runtimeSyncCoordinator().currentAuthoringRevision());
        FAIL() << "reinstall state=" << static_cast<int>(barrier.state)
               << " failure=" << static_cast<int>(barrier.failure);
    }

    EXPECT_GT(runtime->variables.AcceptedCount(), acceptedAfterFirst);
    bool resubmitted = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        ASSERT_EQ(Vn_Update(), VN_OK);
        runtime->nullBackend.Pump(1);
        if (queryVariableCurrent(variableId) == 1.0f) {
            resubmitted = true;
            break;
        }
    }
    EXPECT_TRUE(resubmitted);
    EXPECT_FLOAT_EQ(queryVariableCurrent(variableId), 1.0f);
}

TEST(AudioEnginePreview, ProjectCloseInstallsEmptyWorldBeforeClearingRuntimeObjects)
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
    projectConfig.name = QStringLiteral("EmptyWorldBeforeObjectClear");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    ASSERT_NE(projects.createItem(
                  nullptr, QStringLiteral("Layer"),
                  VNX::ProjectItem::Type::Layer),
              nullptr);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    ASSERT_GT(HarkenEditor::Bridge::runtimeObjectCount(), 0u);

    emptyWorldInstallObserved = false;
    runtimeObjectsWhenEmptyWorldInstalled = 0;
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        observeEmptyWorldInstall);
    ASSERT_TRUE(projects.closeProject());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    EXPECT_TRUE(emptyWorldInstallObserved);
    EXPECT_GT(runtimeObjectsWhenEmptyWorldInstalled, 0u);
    EXPECT_EQ(0u, HarkenEditor::Bridge::runtimeObjectCount());
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
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

TEST(AudioEnginePreview, FailedEmptyWorldInstallQuarantinesRuntimeAndFinishesClose)
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
    projectConfig.name = QStringLiteral("FailedEmptyWorldClose");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        failedWorldInstall);
    ASSERT_TRUE(projects.closeProject());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    EXPECT_EQ(projects.currentProject(), nullptr);
    EXPECT_FALSE(projects.isClosing());
    EXPECT_FALSE(projects.hasPendingDestruction());
    const auto& coordinator = engine.runtimeSyncCoordinator();
    EXPECT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::RuntimeUnavailable);
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
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

TEST(AudioEnginePreview, FullSyncSupersedesPendingWorldBeforeRuntimeClear)
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
    projectConfig.name = QStringLiteral("SupersedeBeforeFullSyncClear");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    observedWorldInstallCallCount = 0;
    installCallCountWhenPendingDiscard = 0;
    pendingDiscardObserved = false;
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        countBackpressuredWorldInstall);
    HarkenEditor::Bridge::setCurrentWorldDiscardObserverForTesting(
        observePendingDiscard);
    const auto allocation = engine.runtimeSyncCoordinator()
        .markCurrentWorldMutation();
    ASSERT_EQ(allocation.state,
              VNX::RuntimeSyncCoordinator::AllocationState::Allocated);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    ASSERT_EQ(observedWorldInstallCallCount, 1u);

    bool finished = false;
    VNX::ProjectManager::syncProjectToRuntimeAsync(
        projects.currentProject(), &projects, [&]() {
            finished = true;
        });

    EXPECT_TRUE(pendingDiscardObserved);
    EXPECT_EQ(installCallCountWhenPendingDiscard, 1u);
    EXPECT_FALSE(finished);
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
}

TEST(AudioEnginePreview, PublishingCompletedAsyncBootstrapPreservesReadyIdentity)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();

    HarkenEditor::Bridge::EngineSessionConfig engineConfig;
    engineConfig.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(engineConfig));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("AsyncBootstrapPublish");
    projectConfig.location = directory.path();
    auto prepared = VNX::ProjectManager::prepareProject(projectConfig);
    ASSERT_NE(prepared.project, nullptr);
    ASSERT_TRUE(projects.commitPreparedProject(std::move(prepared)));

    bool finished = false;
    VNX::ProjectManager::syncProjectToRuntimeAsync(
        projects.currentProject(), &projects, [&]() {
            finished = true;
            projects.publishProjectOpened();
        });
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    for (int attempt = 0; attempt < 32 && !finished; ++attempt)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    ASSERT_TRUE(finished);
    const auto& coordinator = engine.runtimeSyncCoordinator();
    EXPECT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready);
}

TEST(AudioEnginePreview, TemplateStartupCoordinatorReachesReady)
{
    ensureApp();
    if (!qEnvironmentVariableIsSet("HARKEN_TEMPLATE_STARTUP_TEST_CHILD")) {
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("HARKEN_TEMPLATE_STARTUP_TEST_CHILD"),
                           QStringLiteral("1"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                           QStringLiteral("offscreen"));
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--gtest_filter=AudioEnginePreview.TemplateStartupCoordinatorReachesReady"),
                     QStringLiteral("--gtest_color=no")});
        ASSERT_TRUE(child.waitForStarted(5000));
        if (!child.waitForFinished(15000)) {
            child.kill();
            child.waitForFinished();
            FAIL() << child.readAll().toStdString();
        }
        const QByteArray output = child.readAll();
        EXPECT_EQ(child.exitStatus(), QProcess::NormalExit)
            << output.toStdString();
        EXPECT_EQ(child.exitCode(), 0)
            << output.toStdString();
        return;
    }

    AudioEnginePreviewReset reset;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString templatePath =
        QDir(directory.path()).filePath(QStringLiteral("startup-template.json"));
    QFile templateFile(templatePath);
    ASSERT_TRUE(templateFile.open(QIODevice::WriteOnly));
    const QJsonObject templateObject{
        {QStringLiteral("templateFormatVersion"), 2},
        {QStringLiteral("displayName"), QStringLiteral("Startup Template")},
        {QStringLiteral("description"), QString()},
        {QStringLiteral("bus"),
         QJsonObject{
             {QStringLiteral("name"), QStringLiteral("Master Bus")},
             {QStringLiteral("children"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("name"), QStringLiteral("Music")},
                  {QStringLiteral("children"), QJsonArray{}}}}}}},
        {QStringLiteral("attenuation"), QJsonArray{}}};
    ASSERT_NE(templateFile.write(QJsonDocument(templateObject).toJson()), -1);
    templateFile.close();

    VNX::HubClient::Arguments arguments;
    arguments.createProject = true;
    arguments.projectName = QStringLiteral("TemplateCoordinatedStartup");
    arguments.projectLocation = directory.path();
    arguments.projectTemplateId = QStringLiteral("startup-template");
    arguments.projectTemplatePath = templatePath;

    VNX::StartupLoadingWindow loading;
    VNX::StartupCoordinator coordinator(
        *qobject_cast<QApplication*>(qApp), loading,
        std::move(arguments), QString());
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (loading.progressValue() == 100)
            loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    coordinator.start();
    poll.start();
    timeout.start(5000);
    loop.exec();

    EXPECT_EQ(loading.progressValue(), 100);
    EXPECT_EQ(VNX::ProjectManager::instance().thread(), qApp->thread());
    EXPECT_EQ(VNX::AudioEngine::instance().thread(), qApp->thread());
    ASSERT_NE(VNX::ProjectManager::instance().currentProject(), nullptr);
    EXPECT_EQ(VNX::ProjectManager::instance().currentProject()->projectTemplateId,
              QStringLiteral("startup-template"));
}

TEST(AudioEnginePreview, CompiledBusIdentitiesFollowProjectPublicationAndReplacement)
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

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("ChildBusAfterOpen");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    auto* master = projects.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    const harkenstudio::ObjectId masterId = master->runtimeId();
    ASSERT_NE(masterId, harkenstudio::kMasterBusObjectId);
    auto* child = projects.createItem(
        master, QStringLiteral("Music"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(child, nullptr);
    const harkenstudio::ObjectId childId = child->runtimeId();
    ASSERT_NE(childId, harkentest::StudioObjectId(0u));
    ASSERT_NE(childId, masterId);

    const auto idsBeforeOpen = engine.getRuntimeBusIds();
    EXPECT_FALSE(idsBeforeOpen.contains(childId));
    EXPECT_TRUE(idsBeforeOpen.contains(masterId) || idsBeforeOpen.contains(
                    harkenstudio::kMasterBusObjectId));

    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto idsAfterOpen = engine.getRuntimeBusIds();
    EXPECT_TRUE(idsAfterOpen.contains(childId));
    EXPECT_TRUE(idsAfterOpen.contains(masterId));
    EXPECT_TRUE(idsAfterOpen.contains(harkenstudio::kMasterBusObjectId));

    projectConfig.name = QStringLiteral("ReplacementMaster");
    ASSERT_TRUE(projects.createProject(projectConfig));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_NE(projects.getMasterBusItem(), nullptr);
    const auto replacementMasterId = projects.getMasterBusItem()->runtimeId();
    ASSERT_NE(replacementMasterId, masterId);
    const auto idsAfterReplacement = engine.getRuntimeBusIds();
    EXPECT_TRUE(idsAfterReplacement.contains(replacementMasterId));
    EXPECT_TRUE(idsAfterReplacement.contains(harkenstudio::kMasterBusObjectId));
    EXPECT_FALSE(idsAfterReplacement.contains(masterId));
    EXPECT_FALSE(idsAfterReplacement.contains(childId));
}

TEST(AudioEnginePreview, RuntimeUnavailableAsyncBootstrapCompletesWithoutReady)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("UnavailableAsyncBootstrap");
    projectConfig.location = directory.path();
    auto prepared = VNX::ProjectManager::prepareProject(projectConfig);
    ASSERT_NE(prepared.project, nullptr);
    ASSERT_TRUE(projects.commitPreparedProject(std::move(prepared)));

    bool finished = false;
    VNX::ProjectManager::syncProjectToRuntimeAsync(
        projects.currentProject(), &projects, [&]() {
            finished = true;
        });
    for (int attempt = 0; attempt < 32 && !finished; ++attempt)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    EXPECT_TRUE(finished);
    EXPECT_FALSE(projects.runtimePrepared());
    const auto& coordinator = engine.runtimeSyncCoordinator();
    EXPECT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::RuntimeUnavailable);
}

TEST(AudioEnginePreview, ProjectReplacementQueuesBehindPendingCloseBarrier)
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
    projectConfig.name = QStringLiteral("QueuedProjectReplacement");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    ASSERT_NE(projects.createItem(
                  nullptr, QStringLiteral("Layer"),
                  VNX::ProjectItem::Type::Layer),
              nullptr);
    ASSERT_TRUE(projects.saveProject());
    const QString projectPath = projects.currentProject()->projectPath;
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto masterId = projects.currentProject()->busses.front().rootItem->runtimeId();
    const auto foreignBus = harkentest::StudioObjectId(7002);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ToRuntime(foreignBus),harkentest::ToRuntime(harkenstudio::kMasterBusObjectId)));

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    ASSERT_TRUE(projects.loadProject(projectPath));
    EXPECT_EQ(projects.currentProject(), nullptr);
    EXPECT_TRUE(projects.hasPendingDestruction());

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    ASSERT_NE(projects.currentProject(), nullptr);
    EXPECT_EQ(projectPath, projects.currentProject()->projectPath);
    EXPECT_FALSE(projects.hasPendingDestruction());
    EXPECT_TRUE(projects.runtimePrepared());
    auto* runtime = harken::detail::ActiveEngine();
    EXPECT_NE(runtime->buses.FindIndex(harkentest::ToRuntime(masterId)), harken::kInvalidBusIndex);
    EXPECT_NE(runtime->buses.FindIndex(harkentest::ToRuntime(foreignBus)), harken::kInvalidBusIndex);
    ASSERT_TRUE(projects.closeProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(runtime->buses.FindIndex(harkentest::ToRuntime(masterId)), harken::kInvalidBusIndex);
    EXPECT_NE(runtime->buses.FindIndex(harkentest::ToRuntime(foreignBus)), harken::kInvalidBusIndex);
    HarkenEditor::Bridge::resetCurrentWorldSyncStateForTesting();
}

TEST(AudioEnginePreview, RejectedCloseBarrierDoesNotLatchTheNextProjectOpen)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
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
    VNX::ProjectConfig firstConfig;
    firstConfig.name = QStringLiteral("RejectedCloseBarrier");
    firstConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(firstConfig));

    // No processEvents past this point: that is the gtest TearDown-to-SetUp
    // gap, where a queued barrierChanged is never delivered.
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        failedWorldInstall);
    ASSERT_TRUE(projects.closeProject());
    EXPECT_EQ(projects.currentProject(), nullptr);
    EXPECT_FALSE(projects.hasPendingDestruction());
    EXPECT_FALSE(projects.isClosing());

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
    VNX::ProjectConfig secondConfig;
    secondConfig.name = QStringLiteral("RejectedCloseBarrierReopen");
    secondConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(secondConfig));
    ASSERT_NE(projects.currentProject(), nullptr);
    EXPECT_EQ(projects.currentProject()->name, secondConfig.name);
}

TEST(AudioEnginePreview, OutputRestartRestoresTheCurrentProjectForTypedPreview)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    if (VNX::ProjectManager::instance().currentProject() != nullptr)
        VNX::ProjectManager::instance().closeProject();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 4;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath = QDir(directory.path()).filePath(QStringLiteral("restart.wav"));
    const std::vector<float> samples(256, 0.25f);
    writeWavF32(wavPath, samples);

    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("AudioEngineRestart");
    projectConfig.location = directory.path();
    ASSERT_TRUE(VNX::ProjectManager::instance().createProject(projectConfig));
    auto* project = VNX::ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);

    VNX::AssetInfo asset;
    asset.sourceID = 7001;
    asset.relativePath = QStringLiteral("restart.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));

    auto* layer = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = VNX::ProjectManager::instance().createAudioItem(
        layer, QStringLiteral("Sound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);

    VNX::ProjectManager::syncProjectToRuntime(project);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    VnPCMDesc pcm{};
    pcm.structSize = sizeof(pcm);
    pcm.sampleRate = 48000;
    pcm.channels = 1;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = samples.size();
    pcm.samplesInterleaved = samples.data();
    const VnResult registration = Vn_RegisterPCM(harkentest::ToRuntime(sound->runtimeId()), &pcm);
    ASSERT_TRUE(registration == VN_OK || registration == VN_IN_USE);

    const auto beforeRestart = engine.playPreviewPlayable(
        kPreviewSound, sound->runtimeId());
    ASSERT_EQ(beforeRestart.status, kPreviewOk);
    ASSERT_NE(beforeRestart.handle, 0u);
    const auto initialEpoch =
        engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch;

    engine.stopOutput();
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto restartedEpoch =
        engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch;
    EXPECT_GT(restartedEpoch, initialEpoch);

    const auto afterRestart = engine.playPreviewPlayable(
        kPreviewSound, sound->runtimeId());
    ASSERT_EQ(afterRestart.status, kPreviewOk);
    ASSERT_NE(afterRestart.handle, 0u);

    ASSERT_TRUE(engine.reconfigureBufferSize(128));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_GT(engine.runtimeSyncCoordinator().barrier(0).runtimeEpoch,
              restartedEpoch);
    const auto afterReconfigure = engine.playPreviewPlayable(
        kPreviewSound, sound->runtimeId());
    EXPECT_EQ(afterReconfigure.status, kPreviewOk);
    EXPECT_NE(afterReconfigure.handle, 0u);
}

TEST(AudioEnginePreview, Pcm24SoundsAndSequenceQueuePreviewAcrossRuntimeRestart)
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
    projectConfig.name = QStringLiteral("Pcm24PlayablePreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));

    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);

    std::vector<VNX::ProjectItem*> sounds;
    const QString audioSourcesPath = QDir(
        QFileInfo(projects.currentProject()->projectPath).absolutePath())
            .filePath(QStringLiteral("AudioSources"));
    for (uint32_t index = 0; index < 3; ++index) {
        const QString fileName = QStringLiteral("sound%1.wav").arg(index + 1);
        const QString wavPath = QDir(audioSourcesPath).filePath(fileName);
        writeWavPcm24(wavPath, 256);

        VNX::AssetInfo asset;
        asset.sourceID = 7101 + index;
        asset.relativePath = fileName;
        asset.absolutePath = wavPath;
        asset.metadata.sampleRate = 48000;
        asset.metadata.channels = 2;
        asset.metadata.bitDepth = 24;
        asset.metadata.totalSamples = 256;
        asset.metadata.durationSec = 256.0 / 48000.0;
        asset.metadata.fileSize = static_cast<uint64_t>(QFileInfo(wavPath).size());
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
        auto* sound = projects.createAudioItem(
            layer, QStringLiteral("Sound%1").arg(index + 1),
            wavPath, asset.sourceID);
        ASSERT_NE(sound, nullptr);
        sounds.push_back(sound);
    }

    auto* container = projects.createItem(
        layer, QStringLiteral("Container"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    container->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
    container->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
    for (auto* sound : sounds)
        ASSERT_TRUE(projects.moveItem(sound, container));
    auto* source = sounds.front()->comp<VNX::VSound>();
    ASSERT_NE(source, nullptr);
    ASSERT_EQ(source->sources.plain.entries.size(), 1u);
    source->sources.plain.entries.front().trimEndFrame = 256;

    const QUuid containerGuid = container->guid;
    std::vector<QUuid> soundGuids;
    for (const auto* sound : sounds)
        soundGuids.push_back(sound->guid);
    const QString projectPath = projects.currentProject()->projectPath;
    ASSERT_TRUE(projects.saveProject());
    ASSERT_TRUE(projects.loadProject(projectPath));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    sounds.clear();
    for (const QUuid& guid : soundGuids) {
        auto* reloaded = projects.findItemByGuid(guid);
        ASSERT_NE(reloaded, nullptr);
        sounds.push_back(reloaded);
    }
    container = projects.findItemByGuid(containerGuid);
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    for (const auto* sound : sounds) {
        const auto preview = engine.playPreviewPlayable(
            kPreviewSound, sound->runtimeId());
        EXPECT_EQ(preview.status, kPreviewOk);
        EXPECT_NE(preview.handle, 0u);
    }
    const auto beforeRestart = engine.playPreviewPlayable(
        kPreviewContainer, container->runtimeId());
    ASSERT_EQ(beforeRestart.status, kPreviewOk);
    ASSERT_NE(beforeRestart.handle, 0u);

    engine.shutdown();
    ASSERT_TRUE(engine.initialize(config));
    bool asyncSyncFinished = false;
    QEventLoop asyncSyncLoop;
    QTimer asyncSyncTimeout;
    asyncSyncTimeout.setSingleShot(true);
    QObject::connect(&asyncSyncTimeout, &QTimer::timeout,
                     &asyncSyncLoop, &QEventLoop::quit);
    VNX::ProjectManager::syncProjectToRuntimeAsync(
        projects.currentProject(), &asyncSyncLoop, [&]() {
            asyncSyncFinished = true;
            asyncSyncLoop.quit();
        });
    asyncSyncTimeout.start(5000);
    asyncSyncLoop.exec();
    ASSERT_TRUE(asyncSyncFinished);
    const auto afterAsyncStartupSync = engine.playPreviewPlayable(
        kPreviewContainer, container->runtimeId());
    ASSERT_EQ(afterAsyncStartupSync.status, kPreviewOk);
    ASSERT_NE(afterAsyncStartupSync.handle, 0u);

    ASSERT_TRUE(engine.startOutput());
    engine.stopOutput();
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto afterRestart = engine.playPreviewPlayable(
        kPreviewContainer, container->runtimeId());
    ASSERT_EQ(afterRestart.status, kPreviewOk);
    ASSERT_NE(afterRestart.handle, 0u);

    ASSERT_TRUE(engine.reconfigureBufferSize(128));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto afterReconfigure = engine.playPreviewPlayable(
        kPreviewContainer, container->runtimeId());
    EXPECT_EQ(afterReconfigure.status, kPreviewOk);
    EXPECT_NE(afterReconfigure.handle, 0u);
}

TEST(AudioEnginePreview,
     SequenceQueueTransitionsFirstPreviewFromSoundToCompositePiece)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
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

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("QueueCompositePieceTransition");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    auto* project = projects.currentProject();
    ASSERT_NE(project, nullptr);
    const QString audioSourcesPath = QDir(
        QFileInfo(project->projectPath).absolutePath())
            .filePath(QStringLiteral("AudioSources"));

    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    auto* container = projects.createItem(
        layer, QStringLiteral("Queue"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(container, nullptr);

    const QString firstPath =
        QDir(audioSourcesPath).filePath(QStringLiteral("queue-first.wav"));
    writeWavF32(firstPath, std::vector<float>(256, 0.125f));
    VNX::AssetInfo firstAsset;
    firstAsset.sourceID = 7210;
    firstAsset.relativePath = QStringLiteral("queue-first.wav");
    firstAsset.absolutePath = firstPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(firstAsset));
    auto* first = projects.createAudioItem(
        container, QStringLiteral("First"), firstPath,
        firstAsset.sourceID);
    ASSERT_NE(first, nullptr);

    auto* piece = projects.createItem(
        container, QStringLiteral("Composite Piece"),
        VNX::ProjectItem::Type::Piece);
    auto* track = projects.createItem(
        piece, QStringLiteral("Piece Track"),
        VNX::ProjectItem::Type::Track);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(track, nullptr);
    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    const std::array<float, 2> levels{0.25f, 0.5f};
    for (std::uint32_t index = 0; index < levels.size(); ++index) {
        const QString fileName =
            QStringLiteral("queue-piece-%1.wav").arg(index + 1);
        const QString wavPath = QDir(audioSourcesPath).filePath(fileName);
        writeWavF32(wavPath, std::vector<float>(65536, levels[index]));
        VNX::AssetInfo asset;
        asset.sourceID = 7220 + index;
        asset.relativePath = fileName;
        asset.absolutePath = wavPath;
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
        auto* sound = projects.createAudioItem(
            track, QStringLiteral("Piece Sound %1").arg(index + 1),
            wavPath, asset.sourceID);
        ASSERT_NE(sound, nullptr);
        ASSERT_EQ(timeline->clips.items.size(), index + 1);
        EXPECT_EQ(timeline->clips.items.back().targetGuid, sound->guid);
    }

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = VNX::SelectionMode::Sequence;
    selection->playType = VNX::SelectionPlayType::Queue;
    VNX::TransitionRuleDef transitionRule =
        selection->transitions.rules[0];
    transitionRule.exitTiming = VNX::TransitionExitTiming::Immediate;
    transitionRule.sourceFadeOut.durationSeconds = 0.001f;
    transitionRule.targetFadeIn.durationSeconds = 0.001f;
    ASSERT_TRUE(selection->transitions.updateRule(0, transitionRule));

    VNX::ProjectManager::syncProjectToRuntime(project);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->variables.CurrentPlayableWorld().transitionCount, 1u);

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t playingId = audition.playItem(*container);
    ASSERT_NE(playingId, 0u);
    const quintptr previewHandle =
        audition.getAudition(playingId).playbackHandle;
    ASSERT_NE(previewHandle, 0u);
    const float initialPeak = pumpUntilPreviewAudible();
    ASSERT_GT(initialPeak, 0.1f);
    ASSERT_LT(initialPeak, 0.2f);
    const float successorPeak = pumpUntilPreviewPeakAbove(0.7f, 256);
    EXPECT_GT(successorPeak, 0.7f);
    EXPECT_EQ(audition.getAudition(playingId).playbackHandle,
              previewHandle);
}

TEST(AudioEnginePreview, TrackAndPiecePreviewStartOnTheFirstTypedCall)
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

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("TrackPieceFirstCallPreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    auto* project = projects.currentProject();
    ASSERT_NE(project, nullptr);

    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("track.wav"));
    const std::vector<float> samples(512, 0.25f);
    writeWavF32(wavPath, samples);
    VNX::AssetInfo asset;
    asset.sourceID = 7301;
    asset.relativePath = QStringLiteral("track.wav");
    asset.absolutePath = wavPath;
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = samples.size();
    asset.metadata.durationSec =
        static_cast<double>(samples.size()) / 48000.0;
    asset.metadata.fileSize = static_cast<uint64_t>(QFileInfo(wavPath).size());
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));

    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    auto* piece = projects.createItem(
        layer, QStringLiteral("Piece"), VNX::ProjectItem::Type::Piece);
    auto* track = projects.createItem(
        piece, QStringLiteral("Track"), VNX::ProjectItem::Type::Track);
    auto* sound = projects.createAudioItem(
        track, QStringLiteral("Sound"), wavPath, asset.sourceID);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(sound, nullptr);

    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    ASSERT_EQ(timeline->clips.items.size(), 1u);
    EXPECT_EQ(timeline->clips.items.front().targetGuid, sound->guid);

    VNX::ProjectManager::syncProjectToRuntime(project);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    const auto trackPreview = engine.playPreviewPlayable(
        kPreviewTrack, track->runtimeId());
    EXPECT_EQ(kPreviewOk, trackPreview.status);
    EXPECT_NE(0u, trackPreview.handle);
    const auto piecePreview = engine.playPreviewPlayable(
        kPreviewPiece, piece->runtimeId());
    EXPECT_EQ(kPreviewOk, piecePreview.status);
    EXPECT_NE(0u, piecePreview.handle);
}

TEST(AudioEnginePreview, AbsoluteSchedulesOverlapAlignAndLoadFromFormalPacks)
{
    using namespace VNX;
    ensureApp();
    AudioEnginePreviewReset reset;
    auto& engine = AudioEngine::instance();
    engine.shutdown();
    auto& projects = ProjectManager::instance();
    if (projects.currentProject()) projects.closeProject();
    AssetRegistry::instance().clear();
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 32;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("AbsoluteSchedulePlayback");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    auto* project = projects.currentProject();
    ASSERT_NE(project, nullptr);
    const VnBusId authoredMasterId =
        harkentest::ToRuntime(project->busses.front().rootItem->runtimeId());
    ASSERT_NE(authoredMasterId, VN_BUS_MASTER);
    const QString wavPath = QDir(QFileInfo(project->projectPath).absolutePath())
        .filePath(QStringLiteral("AudioSources/absolute.wav"));
    writeWavF32(wavPath, std::vector<float>(7680, 0.125f));
    AssetInfo asset;
    asset.sourceID = 7391;
    asset.relativePath = QStringLiteral("absolute.wav");
    asset.absolutePath = wavPath;
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 7680;
    asset.metadata.durationSec = 0.16;
    asset.metadata.fileSize = static_cast<uint64_t>(QFileInfo(wavPath).size());
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
    auto* outer = projects.createItem(layer, QStringLiteral("Aligned"), ProjectItem::Type::Piece);
    auto* track = projects.createItem(outer, QStringLiteral("Staggered"), ProjectItem::Type::Track);
    auto* sound = projects.createAudioItem(track, QStringLiteral("Pulse"), wavPath, asset.sourceID);
    auto* aligned = projects.createItem(outer, QStringLiteral("SharedOrigin"), ProjectItem::Type::Track);
    auto* alignedSound = projects.createAudioItem(aligned, QStringLiteral("AlignedPulse"), wavPath, asset.sourceID);
    auto* nesting = projects.createItem(outer, QStringLiteral("Nesting"), ProjectItem::Type::Track);
    auto* inner = projects.createItem(nesting, QStringLiteral("Inner"), ProjectItem::Type::Piece);
    auto* innerTrack = projects.createItem(inner, QStringLiteral("InnerTrack"), ProjectItem::Type::Track);
    auto* innerSound = projects.createAudioItem(innerTrack, QStringLiteral("InnerPulse"), wavPath, asset.sourceID);
    auto* action = projects.createItem(nullptr, QStringLiteral("PlayAligned"), ProjectItem::Type::Action);
    auto* stopAction = projects.createItem(nullptr, QStringLiteral("StopAligned"), ProjectItem::Type::Action);
    for (auto* item : {layer, sound, outer, track, aligned, alignedSound, nesting, inner, innerTrack, innerSound, action, stopAction})
        ASSERT_NE(item, nullptr);
    for (auto* owner : {track, aligned, nesting, innerTrack})
        owner->comp<VTrack>()->clips.items.clear();
    const auto appendClip = [&](ProjectItem& owner, ProjectItem& target,
                                int64_t start, int64_t duration) {
        Clip clip;
        clip.clipGuid = QUuid::createUuid();
        clip.targetGuid = target.guid;
        clip.clipGuid = QUuid::createUuid();
        clip.start = start;
        clip.duration = duration;
        auto& clips = owner.comp<VTrack>()->clips;
        EXPECT_TRUE(clips.mintOccurrenceOrdinal(&clip));
        clips.items.push_back(clip);
    };
    for (const int64_t start : {0LL, 80000000LL, 240000000LL})
        appendClip(*track, *sound, start, 160000000);
    appendClip(*aligned, *alignedSound, 0, 160000000);
    appendClip(*nesting, *inner, 160000000, 240000000);
    appendClip(*innerTrack, *innerSound, 80000000, 160000000);
    VActionStep play;
    play.targetObjectID = outer->objectID;
    action->comp<VAction>()->steps = {play};
    VActionStep stop;
    stop.type = VActionStep::Type::Stop;
    stop.targetObjectID = outer->objectID;
    stop.fadeTime = 40.0f;
    stopAction->comp<VAction>()->steps = {stop};
    ProjectManager::syncProjectToRuntime(project);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto capability = RuntimeCapabilityMatrix::assess(*outer, *project,
        RuntimeCapabilityOperation::AuditionPlay);
    ASSERT_TRUE(capability.permitted())
        << QJsonDocument(capability.toJson()).toJson().constData();
    const auto actionCapability = RuntimeCapabilityMatrix::assess(*action, *project,
        RuntimeCapabilityOperation::ActionRegistration);
    ASSERT_TRUE(actionCapability.permitted());
    EXPECT_TRUE(QJsonDocument(actionCapability.toJson()).toJson().contains("scheduling window"));
    const auto capture = [&](uint32_t blockCount = 400) {
        std::vector<float> samples;
        auto* runtime = harken::detail::ActiveEngine();
        for (uint32_t block = 0; block < blockCount; ++block) {
            EXPECT_EQ(Vn_Update(), VN_OK);
            runtime->nullBackend.Pump(1);
            const float* output = runtime->nullBackend.LastBlock();
            for (uint32_t frame = 0; frame < runtime->nullBackend.BlockFrames(); ++frame)
                samples.push_back(output[frame * runtime->nullBackend.Channels()]);
        }
        return samples;
    };
    const auto verify = [&](const std::vector<float>& samples, bool piece) {
        const auto first = std::find_if(samples.begin(), samples.end(),
            [](float value) { return std::abs(value) > 0.00001f; });
        ASSERT_NE(first, samples.end());
        const size_t origin = static_cast<size_t>(std::distance(samples.begin(), first));
        ASSERT_LT(origin + 22000u, samples.size());
        const auto at = [&](size_t milliseconds) { return samples[origin + milliseconds * 48]; };
        const float unit = at(60) / (piece ? 2.0f : 1.0f);
        ASSERT_GT(unit, 0.05f);
        EXPECT_NEAR(at(140), unit * (piece ? 3.0f : 2.0f), 0.003f);
        EXPECT_NEAR(at(200), unit, 0.003f);
        EXPECT_NEAR(at(300), unit * (piece ? 2.0f : 1.0f), 0.003f);
        EXPECT_NEAR(at(390), unit * (piece ? 2.0f : 1.0f), 0.003f);
        EXPECT_NEAR(at(430), 0.0f, 0.00001f);
        EXPECT_NEAR(at(70), unit * (piece ? 2.0f : 1.0f), 0.003f);
        EXPECT_NEAR(at(90), unit * (piece ? 3.0f : 2.0f), 0.003f);
        EXPECT_NEAR(at(230), unit, 0.003f);
        EXPECT_NEAR(at(250), unit * (piece ? 2.0f : 1.0f), 0.003f);
    };
    auto preview = engine.playPreviewPlayable(kPreviewTrack, track->runtimeId());
    ASSERT_EQ(preview.status, kPreviewOk);
    ASSERT_NO_FATAL_FAILURE(verify(capture(), false));
    preview = engine.playPreviewPlayable(kPreviewPiece, outer->runtimeId());
    ASSERT_EQ(preview.status, kPreviewOk);
    ASSERT_NO_FATAL_FAILURE(verify(capture(), true));
    const harkenstudio::ObjectId actionId = action->runtimeId();
    const harkenstudio::ObjectId stopActionId = stopAction->runtimeId();
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(actionId);
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(Vn_PostAction(&post, &firing), VN_OK);
    ASSERT_NE(firing, 0u);
    ASSERT_NO_FATAL_FAILURE(verify(capture(), true));
    const auto verifyStopFade = [&](bool packed) {
        SCOPED_TRACE(packed ? "Pack Action firing stop" : "Current World Stop Action");
        VnActionFiringHandle replay = 0;
        ASSERT_EQ(Vn_PostAction(&post, &replay), VN_OK);
        ASSERT_NE(replay, 0u);
        const auto before = capture(40);
        ASSERT_FALSE(before.empty());
        const float level = before.back();
        ASSERT_GT(level, 0.1f);
        if (packed) {
            ASSERT_EQ(Vn_StopActionFiring(replay, 40), VN_OK);
        } else {
            VnPostActionDesc stopPost{};
            stopPost.structSize = sizeof(stopPost);
            stopPost.actionId = harkentest::ToRuntime(stopActionId);
            VnActionFiringHandle stopping = 0;
            ASSERT_EQ(Vn_PostAction(&stopPost, &stopping), VN_OK);
            ASSERT_NE(stopping, 0u);
        }
        const auto faded = capture();
        EXPECT_GT(faded[20u * 48u], level * 0.1f);
        EXPECT_LT(faded[20u * 48u], level * 0.9f);
        EXPECT_LT(faded[30u * 48u], faded[20u * 48u]);
        EXPECT_TRUE(std::all_of(faded.begin() + 60u * 48u, faded.end(),
            [](float value) { return std::abs(value) < 0.00001f; }));
    };
    ASSERT_NO_FATAL_FAILURE(verifyStopFade(false));
    const QString outputDirectory = directory.filePath(QStringLiteral("packs"));
    ProjectPackExporter exporter;
    ProjectPackExporter::ExportOptions options;
    options.platform = ProjectPlatformSemantics::idString(project->platforms.front().id);
    options.onlyActionStableIds = {actionId};
    const auto exported = exporter.ExportAllPacks(*project, outputDirectory, options);
    ASSERT_TRUE(exported.success) << exported.error.toStdString();
    QJsonArray packs;
    QStringList publicationFiles;
    QString publicationError;
    ASSERT_TRUE(ReadPackPublication(outputDirectory, project->projectId, options.platform,
        &packs, &publicationFiles, &publicationError)) << publicationError.toStdString();
    ASSERT_GE(packs.size(), 2);
    ASSERT_FALSE(project->busses.empty());
    ASSERT_NE(project->busses.front().rootItem, nullptr);
    auto* sendTestOutput = projects.createItem(project->busses.front().rootItem.get(),
        QStringLiteral("SendTestOutput"), ProjectItem::Type::Bus);
    ASSERT_NE(sendTestOutput, nullptr);
    const VnBusId sendTestOutputId = harkentest::ToRuntime(sendTestOutput->runtimeId());
    ASSERT_NE(sendTestOutputId, authoredMasterId);
    const auto originalMainOutput = sound->fieldGroups.mainOutput;
    const auto originalSend = sound->fieldGroups.send;
    sound->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(sendTestOutput->guid), 0.0f}};
    sound->fieldGroups.mainOutput.platforms.clear();
    sound->fieldGroups.send.shared = {
        FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(project->busses.front().rootItem->guid), -12.0f}}}};
    sound->fieldGroups.send.platforms.clear();
    const QString sendOutputDirectory = directory.filePath(QStringLiteral("master-send-packs"));
    const auto sendExported = exporter.ExportAllPacks(*project, sendOutputDirectory, options);
    sound->fieldGroups.send = originalSend;
    sound->fieldGroups.mainOutput = originalMainOutput;
    ASSERT_TRUE(sendExported.success) << sendExported.error.toStdString();
    uint32_t checkedMasterSends = 0;
    for (const auto& artifact : sendExported.artifacts) {
        QFile file(artifact.outputPath);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        harken::PackCanonicalImage canonical;
        ASSERT_EQ(harken::ParsePackCanonicalImage(
            reinterpret_cast<const uint8_t*>(bytes.constData()),
            static_cast<uint64_t>(bytes.size()), &canonical), VN_OK);
        ASSERT_TRUE(canonical.hasPartition);
        harken::CurrentWorldBodyImage decoded;
        ASSERT_TRUE(decoded.Decode(canonical.partitionBody.data(), canonical.partitionBody.size(),
            canonical.partitionHeader.bodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
        ASSERT_NE(decoded.ProductionView(), nullptr);
        const auto& world = *decoded.ProductionView();
        for (uint32_t i = 0; i < world.playablePropertyCount; ++i) {
            const auto& row = world.playableProperties[i];
            if (row.stableId != harkentest::ToRuntime(sound->runtimeId()))
                continue;
            EXPECT_EQ(row.payload.values[6].objectId, sendTestOutputId);
            ASSERT_EQ(row.payload.sendCount, 1u);
            EXPECT_EQ(row.payload.sends[0].targetBusId, authoredMasterId);
            EXPECT_FLOAT_EQ(row.payload.sends[0].sendLevel, -12.0f);
            EXPECT_EQ(harken::ComputePlayablePropertyFingerprintV1(row), row.programSemanticFingerprint);
            ++checkedMasterSends;
        }
        EXPECT_EQ(harken::ComputeCurrentWorldFingerprintV5(world), world.identity.runtimeSemanticFingerprint);
    }
    EXPECT_GT(checkedMasterSends, 0u);
    sound->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(project->busses.front().rootItem->guid), 0.0f}};
    sound->fieldGroups.mainOutput.platforms.clear();
    sound->fieldGroups.send.shared = {
        FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(project->busses.front().rootItem->guid), -12.0f}}}};
    sound->fieldGroups.send.platforms.clear();
    const QString closedSendDirectory = directory.filePath(QStringLiteral("master-send-closed"));
    const auto closedSendExported = exporter.ExportAllPacks(*project, closedSendDirectory, options);
    sound->fieldGroups.send = originalSend;
    sound->fieldGroups.mainOutput = originalMainOutput;
    ASSERT_FALSE(closedSendExported.success);
    EXPECT_TRUE(closedSendExported.error.contains(
                    QStringLiteral("Send target cannot also be the effective Main Output"))
                || closedSendExported.error.contains(QStringLiteral("send==MainOutput")))
        << closedSendExported.error.toStdString();
    projects.closeProject();
    engine.shutdown();
    VnConfig packedConfig{};
    packedConfig.structSize = sizeof(packedConfig);
    packedConfig.sampleRate = 48000;
    packedConfig.blockFrames = 64;
    packedConfig.maxVoices = 32;
    packedConfig.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&packedConfig), VN_OK);
    struct PackedSession { ~PackedSession() { Vn_Term(); } } packedSession;
    std::vector<QJsonObject> orderedPacks;
    for (const auto& value : packs)
        orderedPacks.push_back(value.toObject());
    std::sort(orderedPacks.begin(), orderedPacks.end(),
        [](const QJsonObject& a, const QJsonObject& b) {
            const int aKind = a.value(QStringLiteral("packKind")).toInt();
            const int bKind = b.value(QStringLiteral("packKind")).toInt();
            if (aKind != bKind)
                return aKind < bKind;
            return a.value(QStringLiteral("packStableId")).toString().toULongLong()
                < b.value(QStringLiteral("packStableId")).toString().toULongLong();
        });
    const QByteArray rootUtf8 = QFileInfo(outputDirectory).absoluteFilePath().toUtf8();
    std::vector<QByteArray> paths(orderedPacks.size());
    std::vector<VnCanonicalPackSetMemberV1> members(orderedPacks.size());
    std::vector<VnCanonicalPackSetMemberResultV1> rows(orderedPacks.size());
    for (size_t i = 0; i < orderedPacks.size(); ++i) {
        const auto& pack = orderedPacks[i];
        ASSERT_EQ(pack.value(QStringLiteral("packKind")).toInt(), i == 0 ? 2 : 3);
        const QString fileName = QFileInfo(pack.value(QStringLiteral("path")).toString()).fileName();
        paths[i] = QFileInfo(QDir(outputDirectory).filePath(fileName)).absoluteFilePath().toUtf8();
        members[i].structSize = sizeof(members[i]);
        members[i].flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
        members[i].utf8ContainmentRoot = rootUtf8.constData();
        members[i].utf8Path = paths[i].constData();
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                    : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        if (i == 0) {
            QFile initFile(QString::fromUtf8(paths[i]));
            ASSERT_TRUE(initFile.open(QIODevice::ReadOnly));
            const QByteArray bytes = initFile.readAll();
            harken::PackCanonicalImage initImage;
            ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
                reinterpret_cast<const uint8_t*>(bytes.constData()),
                static_cast<uint64_t>(bytes.size()), &initImage));
            ASSERT_TRUE(initImage.hasPartition);
            ASSERT_TRUE(initImage.hasBusDefinitionSnapshot);
            members[i].packStableId = initImage.partitionHeader.packStableId;
        } else {
            bool ok = false;
            members[i].packStableId =
                pack.value(QStringLiteral("packStableId")).toString().toULongLong(&ok);
            ASSERT_TRUE(ok);
        }
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 activated{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        members.data(), static_cast<uint32_t>(members.size()), rows.data(), activated));
    for (const auto& row : rows)
        ASSERT_NE(row.handle, 0u);
    firing = 0;
    ASSERT_EQ(Vn_PostAction(&post, &firing), VN_OK);
    ASSERT_NE(firing, 0u);
    ASSERT_NO_FATAL_FAILURE(verify(capture(), true));
    ASSERT_NO_FATAL_FAILURE(verifyStopFade(true));
    uint32_t checkedMasterRoutes = 0;
    for (size_t i = 1; i < paths.size(); ++i) {
        QFile packFile(QString::fromUtf8(paths[i]));
        ASSERT_TRUE(packFile.open(QIODevice::ReadOnly));
        const QByteArray bytes = packFile.readAll();
        harken::PackCanonicalImage canonical;
        ASSERT_EQ(harken::ParsePackCanonicalImage(
                      reinterpret_cast<const uint8_t*>(bytes.constData()),
                      static_cast<uint64_t>(bytes.size()), &canonical), VN_OK);
        ASSERT_TRUE(canonical.hasPartition);
        harken::CurrentWorldBodyImage decoded;
        ASSERT_TRUE(decoded.Decode(
            canonical.partitionBody.data(), canonical.partitionBody.size(),
            canonical.partitionHeader.bodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
        ASSERT_NE(decoded.ProductionView(), nullptr);
        const auto& world = *decoded.ProductionView();
        for (uint32_t j = 0; j < world.playablePropertyCount; ++j) {
            const auto& property = world.playableProperties[j];
            if ((property.payload.presentMask & (UINT64_C(1) << 6)) != 0) {
                EXPECT_EQ(property.payload.values[6].objectId, authoredMasterId);
                ++checkedMasterRoutes;
            }
            EXPECT_EQ(harken::ComputePlayablePropertyFingerprintV1(property),
                      property.programSemanticFingerprint);
        }
        EXPECT_EQ(harken::ComputeCurrentWorldFingerprintV5(world),
                  world.identity.runtimeSemanticFingerprint);
    }
    EXPECT_GT(checkedMasterRoutes, 0u);
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

    constexpr harkenstudio::ObjectId foreignBus = harkentest::StudioObjectId(7001);
    ASSERT_FALSE(harkenstudio::IsNull(foreignBus));
    ASSERT_NE(foreignBus, harkenstudio::kMasterBusObjectId);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ToRuntime(foreignBus),harkentest::ToRuntime(harkenstudio::kMasterBusObjectId)));
    VnResult saturation = VN_OK;
    int saturatedCommands = 0;
    while (saturation == VN_OK && saturatedCommands < 512) {
        saturation = Vn_SetBusVolume(harkentest::ToRuntime(foreignBus), -1.0f, 0);
        ++saturatedCommands;
    }
    ASSERT_EQ(saturation, VN_BACKPRESSURE);

    HarkenEditor::AuditionManager audition(&engine);
    int startedCount = 0;
    int failedCount = 0;
    uint32_t failedPlay = 0;
    int32_t failedResult = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&startedCount](uint32_t, const QString&) {
            ++startedCount;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&](uint32_t playingId, const QString&, int32_t result) {
            ++failedCount;
            failedPlay = playingId;
            failedResult = result;
        });
    const uint32_t secondPlay = audition.playItem(*second);
    ASSERT_NE(secondPlay, 0u);
    ASSERT_TRUE(audition.lastPlayPending());
    ASSERT_TRUE(audition.getAudition(secondPlay).previewStartPending);
    EXPECT_EQ(audition.getAudition(secondPlay).playbackHandle, 0u);
    EXPECT_EQ(audition.lastPlayEngineResult(),
              HarkenEditor::Bridge::kEngineBackpressure);
    EXPECT_EQ(startedCount, 0);
    EXPECT_EQ(failedCount, 0);
    EXPECT_EQ(audition.activeAuditionCount(), 1);

    engine.shutdown();
    EXPECT_EQ(failedCount, 1);
    EXPECT_EQ(failedPlay, secondPlay);
    EXPECT_EQ(failedResult, HarkenEditor::Bridge::kEngineNotInit);
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

class ContainerFamilyPreviewStartTest
    : public ::testing::TestWithParam<std::tuple<ContainerPreviewShape, uint32_t>>
{};

TEST_P(ContainerFamilyPreviewStartTest, StartsTheFirstPreviewForEveryShape)
{
    const auto [shape, sourceId] = GetParam();
    expectContainerFamilyPreviewStarts(
        QStringLiteral("ContainerFamilyPreview_%1").arg(sourceId), sourceId, shape);
}

INSTANTIATE_TEST_SUITE_P(EveryShape, ContainerFamilyPreviewStartTest,
    ::testing::Values(
        std::tuple{ContainerPreviewShape::RandomDraw, 7301u},
        std::tuple{ContainerPreviewShape::RandomShuffle, 7302u},
        std::tuple{ContainerPreviewShape::RandomQueue, 7303u},
        std::tuple{ContainerPreviewShape::Sequence, 7304u},
        std::tuple{ContainerPreviewShape::SequenceQueue, 7305u},
        std::tuple{ContainerPreviewShape::VariableDiscrete, 7306u},
        std::tuple{ContainerPreviewShape::VariableContinuous, 7307u}));

TEST(AudioEnginePreview,
     AllSixContainerModesPlaySequentiallyWithoutBackpressure)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    expectAllContainerModesPlaySequentially(directory.path(), false);
}

TEST(AudioEnginePreview, ExportFiveContainerPreviewProjectIfRequested)
{
    const QString dest = qEnvironmentVariable("HARKEN_EXPORT_FIVE_CONTAINERS");
    if (dest.isEmpty())
        GTEST_SKIP() << "HARKEN_EXPORT_FIVE_CONTAINERS is not set";
    const QString abs = QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(dest).absoluteFilePath()));
    const QString tmp = QDir::toNativeSeparators(
        QDir::cleanPath(QDir::temp().absolutePath()));
    const QString prefix = tmp + QDir::separator();
    const auto underTemp = [&](const QString& path) {
        return path.startsWith(prefix, Qt::CaseInsensitive);
    };
    if (!underTemp(abs))
        GTEST_SKIP() << "HARKEN_EXPORT_FIVE_CONTAINERS must be under temp";
    QDir directory(abs);
    if (directory.exists()) {
        if (!directory.entryList(QDir::NoDotAndDotDot | QDir::AllEntries)
                 .isEmpty())
            GTEST_SKIP() << "HARKEN_EXPORT_FIVE_CONTAINERS is not empty";
    } else {
        ASSERT_TRUE(QDir().mkpath(abs));
    }
    const QString canonical = QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(abs).canonicalFilePath()));
    if (canonical.isEmpty() || !underTemp(canonical))
        GTEST_SKIP() << "HARKEN_EXPORT_FIVE_CONTAINERS resolved outside temp";
    expectAllContainerModesPlaySequentially(canonical, true);

    const QString projectRoot = QDir(canonical).filePath(QStringLiteral("five_containers"));
    const QString sources = QDir(projectRoot).filePath(QStringLiteral("AudioSources"));
    QDir().mkpath(sources);
    const QString seed = QStringLiteral(
        "F:/HarkenStudio/.temp/diagnose-random-sequence/ceshishi/AudioSources");
    const QString boar = QDir(seed).filePath(
        QStringLiteral("Ride_FX_Mount_BattleBoar_BCBF_01.wav"));
    const QString gacha = QDir(seed).filePath(
        QStringLiteral("UI_Activity_KnockGZ_Gacha_Grand_01.wav"));
    ASSERT_TRUE(QFile::exists(boar));
    ASSERT_TRUE(QFile::exists(gacha));
    QFile::remove(QDir(sources).filePath(QStringLiteral("family-first.wav")));
    QFile::remove(QDir(sources).filePath(QStringLiteral("family-second.wav")));
    QFile::remove(QDir(sources).filePath(QStringLiteral("variable-control-first.wav")));
    QFile::remove(QDir(sources).filePath(QStringLiteral("variable-control-second.wav")));
    ASSERT_TRUE(QFile::copy(boar, QDir(sources).filePath(QStringLiteral("family-first.wav"))));
    ASSERT_TRUE(QFile::copy(gacha, QDir(sources).filePath(QStringLiteral("family-second.wav"))));
    ASSERT_TRUE(QFile::copy(boar, QDir(sources).filePath(QStringLiteral("variable-control-first.wav"))));
    ASSERT_TRUE(QFile::copy(gacha, QDir(sources).filePath(QStringLiteral("variable-control-second.wav"))));
    ASSERT_TRUE(QFile::exists(QDir(sources).filePath(QStringLiteral("family-first.wav"))));
    ASSERT_TRUE(QFile::exists(QDir(projectRoot).filePath(QStringLiteral("five_containers.hsp"))));
}

TEST(AudioEnginePreview, LoadExportedFiveContainerProjectAndPlayRandomDraw)
{
    const QString dest = qEnvironmentVariable("HARKEN_EXPORT_FIVE_CONTAINERS");
    if (dest.isEmpty())
        GTEST_SKIP() << "HARKEN_EXPORT_FIVE_CONTAINERS is not set";
    const QString projectFile = QDir(dest).filePath(
        QStringLiteral("five_containers/five_containers.hsp"));
    ASSERT_TRUE(QFile::exists(projectFile));
    ASSERT_TRUE(QFile::exists(QDir(dest).filePath(
        QStringLiteral("five_containers/AudioSources/family-first.wav"))));

    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(projects.loadProject(projectFile));
    VNX::ProjectItem* draw = nullptr;
    for (VNX::ProjectFileBinding* binding :
         projects.currentProject()->allFileBindings()) {
        if (binding == nullptr || binding->rootItem == nullptr)
            continue;
        for (auto& child : binding->rootItem->children) {
            if (child && child->name == QStringLiteral("Random_Draw")) {
                draw = child.get();
                break;
            }
        }
    }
    ASSERT_NE(draw, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int32_t failureResult = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&](uint32_t, const QString&, int32_t result) {
            ++failedStarts;
            failureResult = result;
        });
    const uint32_t playingId = audition.playItem(*draw);
    ASSERT_NE(playingId, 0u) << "engine result "
        << static_cast<int>(audition.lastPlayEngineResult());
    EXPECT_NE(audition.lastPlayEngineResult(),
              HarkenEditor::Bridge::kEngineNotInit);
    EXPECT_NE(audition.lastPlayEngineResult(),
              HarkenEditor::Bridge::kEngineBackpressure);
    quintptr handle = audition.getAudition(playingId).playbackHandle;
    for (int attempt = 0; attempt < 64 && handle == 0 && failedStarts == 0;
         ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        handle = audition.getAudition(playingId).playbackHandle;
    }
    EXPECT_EQ(failedStarts, 0) << "engine result " << failureResult;
    EXPECT_NE(handle, 0u);
    EXPECT_EQ(audition.lastPlayEngineResult(), 0);
    EXPECT_GT(pumpUntilPreviewAudible(1024), 0.0f);
}

TEST(AudioEnginePreview, NullMixerNaturalCompletionReleasesSlotOnFirstPoll)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("ShortNaturalCompletionReplay");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7511, 256);
    ASSERT_NE(fixture.container, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&](uint32_t, const QString&, int32_t) { ++failedStarts; });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&](uint32_t, const QString&) { ++successfulStarts; });

    const uint32_t firstId = audition.playItem(*fixture.container);
    ASSERT_NE(firstId, 0u);
    const quintptr firstHandle = audition.getAudition(firstId).playbackHandle;
    ASSERT_NE(firstHandle, 0u);
    VNX::AudioEngine::PreviewPlayableState firstState;
    ASSERT_EQ(engine.queryPreviewPlayable(firstHandle, firstState), kPreviewOk);
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &audition, "pollAuditionStates", Qt::DirectConnection));
    ASSERT_TRUE(previewPlayAccepted(audition.getAudition(firstId)));
    ASSERT_EQ(audition.getAudition(firstId).playbackHandle, firstHandle);
    ASSERT_GT(pumpUntilPreviewAudible(), 0.05f);

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    int32_t firstQuery = kPreviewOk;
    for (int attempt = 0; attempt < 256 && firstQuery != kPreviewStaleHandle;
         ++attempt) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        runtime->nullBackend.Pump(1);
        firstQuery = engine.queryPreviewPlayable(firstHandle, firstState);
    }
    ASSERT_EQ(firstQuery, kPreviewStaleHandle)
        << "lifecycle=" << firstState.lifecycle
        << " childLive=" << firstState.childLiveCount
        << " retiring=" << firstState.childAudibleRetiringCount
        << " future=" << firstState.pendingFutureCount;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &audition, "pollAuditionStates", Qt::DirectConnection));
    ASSERT_FALSE(typedPreviewSlotOccupied(audition));

    const uint32_t secondId = audition.playItem(*fixture.container);
    ASSERT_NE(secondId, 0u);
    const auto second = audition.getAudition(secondId);
    EXPECT_TRUE(previewPlayAccepted(second));
    EXPECT_NE(second.playbackHandle, firstHandle);
    EXPECT_EQ(second.previewEngineResult, 0);
    EXPECT_TRUE(second.previewReason.isEmpty());
    EXPECT_EQ(successfulStarts, 2);
    EXPECT_EQ(failedStarts, 0);
    EXPECT_GT(pumpUntilPreviewAudible(), 0.05f);
}

TEST(AudioEnginePreview,
     NaturalCompletionReplaysSameContainerOnFirstIntent)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("NaturalCompletionReplay");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7510);
    ASSERT_NE(fixture.container, nullptr);
    auto* container = fixture.container;
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = VNX::SelectionMode::Sequence;
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t firstId = audition.playItem(*container);
    ASSERT_NE(firstId, 0u);
    const quintptr firstHandle = audition.getAudition(firstId).playbackHandle;
    ASSERT_NE(firstHandle, 0u);
    QThread::msleep(100);

    VNX::AudioEngine::PreviewPlayableState firstState;
    int32_t firstQuery = -1;
    bool sawNaturalCompletion = false;
    for (int attempt = 0; attempt < 2048; ++attempt) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        runtime->nullBackend.Pump(1);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        firstQuery = engine.queryPreviewPlayable(firstHandle, firstState);
        if (firstQuery == kPreviewStaleHandle)
            break;
        if (firstQuery == kPreviewOk
            && (firstState.lifecycle == kPreviewLogicalComplete
                || firstState.lifecycle == kPreviewAudibleRetiring)) {
            sawNaturalCompletion = true;
            break;
        }
        QThread::msleep(1);
    }
    ASSERT_TRUE(sawNaturalCompletion || firstQuery == kPreviewStaleHandle)
        << "query=" << firstQuery
        << " lifecycle=" << firstState.lifecycle
        << " childLive=" << firstState.childLiveCount
        << " retiring=" << firstState.childAudibleRetiringCount
        << " future=" << firstState.pendingFutureCount;
    for (int attempt = 0; attempt < 2048 && firstQuery != kPreviewStaleHandle;
         ++attempt) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        runtime->nullBackend.Pump(1);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        firstQuery = engine.queryPreviewPlayable(firstHandle, firstState);
        QThread::msleep(1);
    }
    ASSERT_EQ(firstQuery, kPreviewStaleHandle)
        << "lifecycle=" << firstState.lifecycle
        << " childLive=" << firstState.childLiveCount
        << " retiring=" << firstState.childAudibleRetiringCount
        << " future=" << firstState.pendingFutureCount;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &audition, "pollAuditionStates", Qt::DirectConnection));
    ASSERT_FALSE(typedPreviewSlotOccupied(audition));

    int failedStarts = 0;
    int successfulStarts = 0;
    int32_t failureResult = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&](uint32_t, const QString&, int32_t result) {
            ++failedStarts;
            failureResult = result;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&](uint32_t, const QString&) { ++successfulStarts; });

    const uint32_t secondId = audition.playItem(*container);
    ASSERT_NE(secondId, 0u);
    const auto secondInitial = audition.getAudition(secondId);
    EXPECT_FALSE(secondInitial.previewStartPending);
    EXPECT_EQ(secondInitial.previewEngineResult, 0);
    EXPECT_TRUE(secondInitial.previewReason.isEmpty());
    for (int attempt = 0; attempt < 600 && failedStarts == 0
         && successfulStarts == 0; ++attempt) {
        pumpPreviewTransport(engine, audition);
        QThread::msleep(1);
    }

    EXPECT_EQ(failedStarts, 0) << "engine result " << failureResult;
    EXPECT_EQ(successfulStarts, 1);
    EXPECT_TRUE(previewPlayAccepted(audition.getAudition(secondId)));
}

TEST(AudioEnginePreview, PlayResolvedReplaceDoesNotCallStopAll)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("PreviewReplaceNoStopAll");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto containers = createDualSequencePreview(projects, 7501);
    ASSERT_NE(containers.first, nullptr);
    ASSERT_NE(containers.second, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t firstId = audition.playItem(*containers.first);
    ASSERT_NE(firstId, 0u);
    ASSERT_NE(audition.getAudition(firstId).playbackHandle, 0u);
    const uint32_t ghostId = audition.registerExternalPlayback(
        QStringLiteral("EventGhost"), 0x1234);
    ASSERT_NE(ghostId, 0u);
    const uint32_t secondId = audition.playItem(*containers.second);
    ASSERT_NE(secondId, 0u);
    EXPECT_EQ(audition.getAudition(ghostId).playingId, ghostId)
        << "playResolved called stopAll";
    EXPECT_EQ(audition.getAudition(ghostId).mode,
              HarkenEditor::AuditionMode::NamedPlayback);
    EXPECT_EQ(previewSlotCount(audition), 2);
    EXPECT_EQ(previewHandleCount(audition), 2);
    EXPECT_NE(secondId, firstId);
    EXPECT_NE(audition.getAudition(firstId).playbackHandle, 0u);
    EXPECT_NE(audition.getAudition(secondId).playbackHandle, 0u);
    EXPECT_NE(audition.getAudition(secondId).playbackHandle,
              audition.getAudition(firstId).playbackHandle);
}

TEST(AudioEnginePreview, ReplaceBeforeRetiredDoesNotStartSecondHandle)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("PreviewReplaceSingleHandle");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto containers = createDualSequencePreview(projects, 7502);
    ASSERT_NE(containers.first, nullptr);
    ASSERT_NE(containers.second, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t firstId = audition.playItem(*containers.first);
    ASSERT_NE(firstId, 0u);
    const quintptr firstHandle = audition.getAudition(firstId).playbackHandle;
    ASSERT_NE(firstHandle, 0u);
    const uint32_t secondId = audition.playItem(*containers.second);
    ASSERT_NE(secondId, 0u);
    EXPECT_NE(secondId, firstId);
    EXPECT_EQ(previewSlotCount(audition), 2);
    EXPECT_EQ(previewHandleCount(audition), 2);
    const auto firstAfter = audition.getAudition(firstId);
    const auto after = audition.getAudition(secondId);
    EXPECT_EQ(firstAfter.playbackHandle, firstHandle);
    EXPECT_TRUE(firstAfter.bIsPlaying);
    EXPECT_FALSE(after.revisionPending);
    EXPECT_NE(after.playbackHandle, 0u);
    EXPECT_NE(after.playbackHandle, firstHandle);
    EXPECT_TRUE(after.bIsPlaying);
    EXPECT_EQ(after.state, 1);
    EXPECT_NE(after.previewReason, QStringLiteral("occupancy"));
}

TEST(AudioEnginePreview, PendingPreviewDoesNotReportPlaying)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("PreviewPendingNotPlaying");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto containers = createDualSequencePreview(projects, 7503);
    ASSERT_NE(containers.first, nullptr);
    ASSERT_NE(containers.second, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    VNX::ActionManager::instance().clearAllForTesting();
    VNX::registerAuditionActions(
        VNX::ActionManager::instance(), &audition, &engine);
    const uint32_t firstId = audition.playItem(*containers.first);
    ASSERT_NE(firstId, 0u);
    ASSERT_NE(audition.getAudition(firstId).playbackHandle, 0u);

    const auto played = VNX::ActionManager::instance().execute(
        QStringLiteral("audition_play"),
        QJsonObject{{QStringLiteral("object_path"),
                     containers.second->name}},
        VNX::ActionSource::IPC);
    ASSERT_TRUE(played.success) << played.errorMessage.toStdString();
    EXPECT_FALSE(played.data.value(QStringLiteral("pending")).toBool());
    EXPECT_EQ(played.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("playing"));
    EXPECT_NE(played.data.value(QStringLiteral("reason")).toString(),
              QStringLiteral("occupancy"));
    EXPECT_EQ(audition.getAudition(firstId).state, 1);
    EXPECT_TRUE(audition.getAudition(firstId).bIsPlaying);

    VNX::IPCServer ipc(&engine);
    const QJsonObject ipcPlay = ipc.dispatchCommand(
        QStringLiteral("audition_play"),
        QJsonObject{{QStringLiteral("object_path"),
                     containers.second->name}});
    EXPECT_EQ(ipcPlay.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject ipcResult =
        ipcPlay.value(QStringLiteral("result")).toObject();
    EXPECT_FALSE(ipcResult.value(QStringLiteral("pending")).toBool());
    EXPECT_EQ(ipcResult.value(QStringLiteral("state")).toString(),
              QStringLiteral("playing"));
    EXPECT_TRUE(ipcResult.contains(QStringLiteral("engine_result")));
    EXPECT_TRUE(ipcResult.contains(QStringLiteral("playbackHandle")));
    EXPECT_TRUE(ipcResult.contains(QStringLiteral("lifecycle")));
    EXPECT_TRUE(ipcResult.contains(QStringLiteral("childLiveCount")));
    EXPECT_TRUE(ipcResult.contains(QStringLiteral("childAudibleRetiringCount")));
    EXPECT_TRUE(ipcResult.contains(QStringLiteral("pendingFutureCount")));

    ipc.setMainWindow(nullptr);
    VNX::ActionManager::instance().clearAllForTesting();
}

TEST(AudioEnginePreview, RehearsalFailureIsNotOccupancyAndAppearsInIpcError)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("PreviewRehearsalNotOccupancy");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto fixture = createVariableControlPreviewFixture(projects, 7504);
    ASSERT_NE(fixture.container, nullptr);
    ASSERT_NE(fixture.step, nullptr);
    ContainerActionReset actions(projects);
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success) << modeChanged.errorMessage.toStdString();
    const auto gridChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(fixture.container->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), gridSelectionsJson(*fixture.container->comp<VNX::VSelection>(), discreteGridJson(fixture))}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(gridChanged.success) << gridChanged.errorMessage.toStdString();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    const harkenstudio::ObjectId variableId = fixture.step->runtimeId();
    ASSERT_NE(variableId, harkentest::StudioObjectId(0u));
    const auto observation = harken::ObserveVariableCallAt(
        runtime->variables.CurrentWorldGeneration(),
        runtime->variables.CurrentTransportEpoch(), runtime->config.blockFrames,
        runtime->timelinePool.WindowStartFrame(),
        runtime->timelinePool.PublishCutoffFrame());
    std::vector<harken::PreparedVariableWriteToken> pendingWrites(
        runtime->variables.PendingCapacity());
    for (auto& write : pendingWrites) {
        ASSERT_EQ(VN_OK, runtime->variables.PrepareGlobalWrite(
            harkentest::ToRuntime(variableId), 1.0f, observation, 0, &write).result);
    }
    ASSERT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED,
              Vn_SetValue(harkentest::ToRuntime(variableId), 1.0f));
    ASSERT_TRUE(projects.variableRehearsal().setOverride(
        fixture.step->guid, 2.0f));

    HarkenEditor::AuditionManager audition(&engine);
    VNX::registerAuditionActions(
        VNX::ActionManager::instance(), &audition, &engine);
    const auto played = VNX::ActionManager::instance().execute(
        QStringLiteral("audition_play"),
        QJsonObject{{QStringLiteral("object_path"),
                     fixture.container->name}},
        VNX::ActionSource::IPC);
    EXPECT_TRUE(played.success) << played.errorMessage.toStdString();
    EXPECT_NE(played.data.value(QStringLiteral("reason")).toString(),
              QStringLiteral("rehearsal"));
    EXPECT_NE(played.data.value(QStringLiteral("reason")).toString(),
              QStringLiteral("occupancy"));
    const uint32_t ipcPlayingId =
        static_cast<uint32_t>(played.data.value(QStringLiteral("auditionID")).toInt());
    ASSERT_NE(ipcPlayingId, 0u);
    for (auto& write : pendingWrites)
        runtime->variables.CancelPreparedWrite(&write);
    for (int attempt = 0;
         attempt < 16
         && audition.getAudition(ipcPlayingId).playbackHandle == 0; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        runtime->nullBackend.Pump(1);
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "retryPendingPreviewStarts", Qt::DirectConnection));
    }
    ASSERT_NE(audition.getAudition(ipcPlayingId).playbackHandle, 0u);
    EXPECT_TRUE(audition.getAudition(ipcPlayingId).bIsPlaying);
    EXPECT_EQ(audition.activeAuditionCount(), 1);

    VNX::IPCServer ipc(&engine);
    const QJsonObject ipcPlay = ipc.dispatchCommand(
        QStringLiteral("audition_play"),
        QJsonObject{{QStringLiteral("object_path"),
                     fixture.container->name}});
    EXPECT_EQ(ipcPlay.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject ipcResult =
        ipcPlay.value(QStringLiteral("result")).toObject();
    EXPECT_NE(ipcResult.value(QStringLiteral("reason")).toString(),
              QStringLiteral("rehearsal"));
    EXPECT_NE(ipcResult.value(QStringLiteral("reason")).toString(),
              QStringLiteral("occupancy"));
    VNX::ActionManager::instance().clearAllForTesting();
}

TEST(AudioEnginePreview, NullMixerPumpReplaceStartsNextAfterRetired)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("PreviewReplaceAfterRetired");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto containers = createDualSequencePreview(projects, 7505);
    ASSERT_NE(containers.first, nullptr);
    ASSERT_NE(containers.second, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t firstId = audition.playItem(*containers.first);
    ASSERT_NE(firstId, 0u);
    const quintptr firstHandle = audition.getAudition(firstId).playbackHandle;
    ASSERT_NE(firstHandle, 0u);
    const uint32_t secondId = audition.playItem(*containers.second);
    ASSERT_NE(secondId, 0u);
    EXPECT_NE(secondId, firstId);
    EXPECT_EQ(previewHandleCount(audition), 2);
    quintptr nextHandle = audition.getAudition(secondId).playbackHandle;
    EXPECT_NE(audition.lastPlayReason(), QStringLiteral("stop-stuck"));
    EXPECT_FALSE(audition.getAudition(secondId).previewStopStuck);
    EXPECT_NE(nextHandle, 0u);
    EXPECT_NE(nextHandle, firstHandle);
    EXPECT_EQ(audition.getAudition(firstId).playbackHandle, firstHandle);
    EXPECT_EQ(previewHandleCount(audition), 2);
    EXPECT_EQ(audition.getAudition(secondId).objectName,
              containers.second->name);
    EXPECT_TRUE(audition.getAudition(secondId).bIsPlaying);
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.05f);
}

TEST(AudioEnginePreview, StopStuckWithoutMixerPumpKeepsSingleHandle)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("PreviewStopStuckNoPump");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    const auto containers = createDualSequencePreview(projects, 7506);
    ASSERT_NE(containers.first, nullptr);
    ASSERT_NE(containers.second, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    const uint32_t firstId = audition.playItem(*containers.first);
    ASSERT_NE(firstId, 0u);
    const quintptr firstHandle = audition.getAudition(firstId).playbackHandle;
    ASSERT_NE(firstHandle, 0u);
    const uint32_t secondId = audition.playItem(*containers.second);
    ASSERT_NE(secondId, 0u);
    EXPECT_NE(secondId, firstId);
    EXPECT_EQ(previewHandleCount(audition), 2);
    EXPECT_NE(audition.getAudition(secondId).playbackHandle, 0u);
    EXPECT_NE(audition.getAudition(secondId).playbackHandle, firstHandle);
    EXPECT_EQ(audition.getAudition(firstId).playbackHandle, firstHandle);
    EXPECT_TRUE(audition.getAudition(secondId).bIsPlaying);
    EXPECT_FALSE(audition.getAudition(secondId).previewStopStuck);
    EXPECT_NE(audition.lastPlayReason(), QStringLiteral("stop-stuck"));
}

namespace {

class FieldGroupNamePreview : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureApp();
        auto& engine = VNX::AudioEngine::instance();
        engine.shutdown();
        auto& projects = VNX::ProjectManager::instance();
        if (projects.currentProject())
            projects.closeProject();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        VNX::AssetRegistry::instance().clear();
        VNX::PlatformContextService::instance().setStorageForTesting(
            std::make_unique<VNX::MemorySettingsStorage>());
        HarkenEditor::Bridge::EngineSessionConfig config;
        config.sampleRate = 48000;
        config.bufferSize = 64;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(engine.initialize(config));
        ASSERT_TRUE(engine.startOutput());
        ASSERT_TRUE(directory.isValid());
        VNX::ProjectConfig projectConfig;
        projectConfig.name = QStringLiteral("FieldGroupNamePreview");
        projectConfig.location = directory.path();
        ASSERT_TRUE(projects.createProject(projectConfig));
        projects.setProjectPlatformsInternal(HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")}));
        VNX::PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
        const QString path = QDir(directory.path()).filePath(QStringLiteral("groups.wav"));
        writeWavF32(path, std::vector<float>(65536, 0.25f));
        VNX::AssetInfo asset;
        asset.sourceID = 78421;
        asset.relativePath = QStringLiteral("groups.wav");
        asset.absolutePath = path;
        ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
        parent = projects.createItem(nullptr, QStringLiteral("Parent"), VNX::ProjectItem::Type::Layer);
        otherParent = projects.createItem(nullptr, QStringLiteral("Other"), VNX::ProjectItem::Type::Layer);
        ASSERT_NE(parent, nullptr);
        ASSERT_NE(otherParent, nullptr);
        sound = projects.createAudioItem(parent, QStringLiteral("Sound"), path, asset.sourceID);
        ASSERT_NE(sound, nullptr);
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerFieldGroupActions(VNX::ActionManager::instance(), &projects);
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        VNX::ProjectManager::instance().closeProject();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        VNX::AssetRegistry::instance().clear();
    }

    void expectSpatialized(bool expected, float distanceScale = 1.0f)
    {
        ASSERT_TRUE(serviceUntilCurrentWorldReady(VNX::AudioEngine::instance()));
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        const auto world = runtime->variables.CurrentPlayableWorld();
        const harken::PlayablePropertyProgramViewV1* definition = nullptr;
        for (uint32_t i = 0; i < world.propertyCount; ++i)
            if (world.properties[i].stableId == sound->runtimeId())
                definition = &world.properties[i];
        ASSERT_NE(definition, nullptr);
        harken::ResolvedPlayablePropertyCutStorageV1 cut;
        ASSERT_TRUE(cut.Reset({}));
        harken::PlayableRefV1 root{};
        root.kind = harken::PlayableKindV1::Sound;
        root.stableId = harkentest::ToRuntime(sound->runtimeId());
        root.expectedProgramSemanticFingerprint = definition->programSemanticFingerprint;
        root.contentRevision = definition->contentRevision;
        root.routeStableId = definition->routeStableId;
        root.compatibilityFingerprint = definition->compatibilityFingerprint;
        ASSERT_EQ(cut.Resolve(world, root, nullptr, 0),
                  harken::ResolvedPlayablePropertyCutStatusV1::Ok);
        EXPECT_EQ(cut.View().spatial.is3DEnabled, expected);
        EXPECT_FLOAT_EQ(cut.View().spatial.attenuationDistanceScaling, distanceScale);
        const auto started = VNX::AudioEngine::instance().playByNameDetailed(sound->name);
        const auto handle = static_cast<uint64_t>(started.playbackHandle);
        ASSERT_NE(handle, 0u) << "result=" << started.engineResult;
        EXPECT_EQ(HarkenEditor::Bridge::Detail::runtimeSetPlaybackPosition(
                      handle, 1.0f, 0.0f, 1.0f), expected);
        ASSERT_GT(pumpUntilPreviewAudible(), 0.0f);
        EXPECT_TRUE(HarkenEditor::Bridge::Detail::runtimeStopPlayback(handle, 0));
        for (int block = 0; block < 32
             && HarkenEditor::Bridge::Detail::runtimeOwnsPlayback(handle); ++block) {
            runtime->nullBackend.Pump(1);
            QMetaObject::invokeMethod(&VNX::AudioEngine::instance(),
                                     "onRuntimeServiceTimer", Qt::DirectConnection);
        }
        EXPECT_FALSE(HarkenEditor::Bridge::Detail::runtimeOwnsPlayback(handle));
    }

    void retirePlayback(const VNX::AudioEngine::PlaybackStartResult& started)
    {
        const auto handle = static_cast<uint64_t>(started.playbackHandle);
        if (handle == 0)
            return;
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        EXPECT_TRUE(HarkenEditor::Bridge::Detail::runtimeStopPlayback(handle, 0));
        for (int block = 0; block < 32
             && HarkenEditor::Bridge::Detail::runtimeOwnsPlayback(handle); ++block) {
            runtime->nullBackend.Pump(1);
            QMetaObject::invokeMethod(&VNX::AudioEngine::instance(),
                                     "onRuntimeServiceTimer", Qt::DirectConnection);
        }
        EXPECT_FALSE(HarkenEditor::Bridge::Detail::runtimeOwnsPlayback(handle));
    }

    void expectGroups(VNX::InspectorPanel& inspector,
                      const VNX::EffectiveDefinitionFieldGroups& expected)
    {
        const auto& playback = expected;
        ASSERT_NO_FATAL_FAILURE(expectSpatialized(
            playback.spatialRating.spatialIs3DEnabled,
            playback.spatialRating.spatialAttenuationDistanceScaling));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto& projects = VNX::ProjectManager::instance();
        const auto& context = VNX::PlatformContextService::instance().context();
        for (const auto& [group, payload] : std::array{
                 std::pair{VNX::FieldGroupId::MainOutput,
                           VNX::FieldGroupPayload{expected.mainOutput}},
                 std::pair{VNX::FieldGroupId::Send,
                           VNX::FieldGroupPayload{expected.send}},
                 std::pair{VNX::FieldGroupId::SpatialRating,
                           VNX::FieldGroupPayload{expected.spatialRating}}}) {
            const auto view = VNX::buildFieldGroupInspectorView(
                *projects.currentProject(), sound->guid, group, context);
            ASSERT_TRUE(view.valid) << view.diagnostics.join(';').toStdString();
            ASSERT_EQ(view.perScope.size(), 1u);
            EXPECT_EQ(view.perScope.front().resolved.payload, payload);
            auto* inherit = inspector.findChild<QCheckBox*>(
                QStringLiteral("inherit_")
                + QString::fromLatin1(VNX::FieldGroupName(group).data()));
            ASSERT_NE(inherit, nullptr);
            EXPECT_EQ(inherit->checkState(), Qt::Checked);
        }
        const auto output = projects.findItemByGuid(
            VNX::fromFieldGroupGuid(expected.mainOutput.mainOutputBusRef));
        const auto attenuation = projects.findItemByGuid(
            VNX::fromFieldGroupGuid(expected.spatialRating.spatialAttenuationRef));
        ASSERT_NE(output, nullptr);
        if (expected.spatialRating.spatialAttenuationRef != harkenprojectschema::AuthoringGuid{})
            ASSERT_NE(attenuation, nullptr);
        auto* outputRef = inspector.findChild<VNX::ObjectRefWidget*>("prop_mainOutputBusRef");
        auto* attenuationRef = inspector.findChild<VNX::ObjectRefWidget*>("prop_spatialAttenuationRef");
        ASSERT_NE(outputRef, nullptr);
        ASSERT_NE(attenuationRef, nullptr);
        EXPECT_EQ(outputRef->currentObjectID(), output->objectID);
        EXPECT_EQ(attenuationRef->currentObjectID(), attenuation ? attenuation->objectID : 0u);
        EXPECT_TRUE(outputRef->isReadOnly());
        EXPECT_TRUE(attenuationRef->isReadOnly());
        auto* spatialized = inspector.findChild<QCheckBox*>("prop_spatialIs3DEnabled");
        ASSERT_NE(spatialized, nullptr);
        EXPECT_EQ(spatialized->isChecked(), expected.spatialRating.spatialIs3DEnabled);
        EXPECT_FALSE(spatialized->isEnabled());
        for (const auto& [field, value] : std::array{
                 std::pair{"outputBusVolume", expected.mainOutput.outputBusVolume},
                 std::pair{"spatialPanning", expected.spatialRating.spatialPanning},
                 std::pair{"spatialAttenuationDistanceScaling",
                           expected.spatialRating.spatialAttenuationDistanceScaling}}) {
            auto* widget = inspector.findChild<VNX::ParameterSlider*>(
                QStringLiteral("prop_") + field);
            ASSERT_NE(widget, nullptr);
            EXPECT_NEAR(widget->value(), value, 0.001);
            EXPECT_FALSE(widget->isEnabled());
        }
        auto* sendList = inspector.findChild<QWidget*>("prop_sends");
        ASSERT_NE(sendList, nullptr);
        const auto sendRefs = sendList->findChildren<VNX::ObjectRefWidget*>();
        QList<QLabel*> sendLevels;
        for (auto* label : sendList->findChildren<QLabel*>())
            if (label->property("roValue").toBool()) sendLevels.push_back(label);
        ASSERT_EQ(sendRefs.size(), expected.send.sends.size());
        ASSERT_EQ(sendLevels.size(), expected.send.sends.size());
        for (qsizetype i = 0; i < sendRefs.size(); ++i) {
            const auto* target = projects.findItemByGuid(
                VNX::fromFieldGroupGuid(expected.send.sends[i].targetBusID));
            ASSERT_NE(target, nullptr);
            EXPECT_EQ(sendRefs[i]->currentObjectID(), target->objectID);
            EXPECT_TRUE(sendRefs[i]->isReadOnly());
            EXPECT_EQ(sendLevels[i]->text(),
                      QString::number(expected.send.sends[i].sendLevel, 'f', 1));
        }
        const auto world = harken::detail::ActiveEngine()->variables.CurrentPlayableWorld();
        const harken::PlayablePropertyProgramViewV1* definition = nullptr;
        for (uint32_t i = 0; i < world.propertyCount; ++i)
            if (world.properties[i].stableId == sound->runtimeId())
                definition = &world.properties[i];
        ASSERT_NE(definition, nullptr);
        harken::PlayableRefV1 root{};
        root.kind = harken::PlayableKindV1::Sound;
        root.stableId = harkentest::ToRuntime(sound->runtimeId());
        root.expectedProgramSemanticFingerprint = definition->programSemanticFingerprint;
        root.contentRevision = definition->contentRevision;
        root.routeStableId = definition->routeStableId;
        root.compatibilityFingerprint = definition->compatibilityFingerprint;
        harken::ResolvedPlayablePropertyCutStorageV1 cut;
        ASSERT_TRUE(cut.Reset({}));
        ASSERT_EQ(cut.Resolve(world, root, nullptr, 0),
                  harken::ResolvedPlayablePropertyCutStatusV1::Ok);
        const auto* playbackOutput = projects.findItemByGuid(
            VNX::fromFieldGroupGuid(playback.mainOutput.mainOutputBusRef));
        const auto* playbackAttenuation = projects.findItemByGuid(
            VNX::fromFieldGroupGuid(playback.spatialRating.spatialAttenuationRef));
        ASSERT_NE(playbackOutput, nullptr);
        if (playback.spatialRating.spatialAttenuationRef != harkenprojectschema::AuthoringGuid{})
            ASSERT_NE(playbackAttenuation, nullptr);
        EXPECT_EQ(cut.View().mainOutputBusRef, playbackOutput->runtimeId());
        EXPECT_FLOAT_EQ(cut.View().outputBusVolumeDb, playback.mainOutput.outputBusVolume);
        EXPECT_FLOAT_EQ(cut.View().spatial.panning, playback.spatialRating.spatialPanning);
        EXPECT_FLOAT_EQ(cut.View().spatial.spread, 0.0f);
        EXPECT_EQ(cut.View().spatial.attenuationRef, playbackAttenuation ? playbackAttenuation->runtimeId() : harkenstudio::ObjectId{});
        ASSERT_EQ(cut.View().sendCount, playback.send.sends.size());
        for (uint32_t i = 0; i < cut.View().sendCount; ++i) {
            const auto* target = projects.findItemByGuid(
                VNX::fromFieldGroupGuid(playback.send.sends[i].targetBusID));
            ASSERT_NE(target, nullptr);
            EXPECT_EQ(cut.View().sends[i].targetBusId, target->runtimeId());
            EXPECT_FLOAT_EQ(cut.View().sends[i].sendLevel, playback.send.sends[i].sendLevel);
        }
    }

    QTemporaryDir directory;
    VNX::ProjectItem* parent = nullptr;
    VNX::ProjectItem* otherParent = nullptr;
    VNX::ProjectItem* sound = nullptr;
};

}

TEST_F(FieldGroupNamePreview, AncestorEditsMovesAndUndoRefreshTheNextNamePlay)
{
    auto& projects = VNX::ProjectManager::instance();
    auto groups = parent->fieldGroups;
    groups.spatialRating.shared = {VNX::FieldGroupMode::Local, VNX::SpatialRatingFieldGroup{}};
    ASSERT_TRUE(projects.restoreFieldGroupsInternal(parent, groups));
    expectSpatialized(false);
    const auto revision = VNX::AudioEngine::instance().runtimeSyncCoordinator().currentAuthoringRevision();
    const auto changed = VNX::ActionManager::instance().execute(
        QStringLiteral("set_field_group_field"),
        QJsonObject{{"definition_guid", parent->guid.toString(QUuid::WithoutBraces)},
                    {"group", "spatialRating"}, {"scope", "Shared"},
                    {"field", "spatial.is3DEnabled"}, {"value", true}});
    ASSERT_TRUE(changed.success) << changed.errorMessage.toStdString();
    EXPECT_GT(VNX::AudioEngine::instance().runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    const auto pending = VNX::AudioEngine::instance().playByNameDetailed(sound->name);
    EXPECT_EQ(pending.engineResult, HarkenEditor::Bridge::kEngineOk);
    EXPECT_NE(pending.playbackHandle, 0u);
    ASSERT_NO_FATAL_FAILURE(retirePlayback(pending));
    expectSpatialized(true);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    expectSpatialized(false);
    VNX::UndoRedoManager::instance().undoStack()->redo();
    expectSpatialized(true);
    ASSERT_TRUE(projects.moveItem(sound, otherParent));
    expectSpatialized(false);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    expectSpatialized(true);
    ASSERT_TRUE(projects.deleteItem(parent));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(VNX::AudioEngine::instance()));
    EXPECT_FALSE(HarkenEditor::Bridge::runtimeObjectExists(sound->runtimeId()));
    EXPECT_EQ(HarkenEditor::Bridge::Detail::runtimePlayByName(
                  sound->name, false, 0.0f, 0.0f, 0.0f), 0u);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    expectSpatialized(true);
}

TEST_F(FieldGroupNamePreview, PrimaryPlatformChangesRefreshRatingAndRoutingQueries)
{
    auto& projects = VNX::ProjectManager::instance();
    auto* sendBus = projects.createItem(projects.getMasterBusItem(), QStringLiteral("AuxSend"),
                                        VNX::ProjectItem::Type::Bus);
    ASSERT_NE(sendBus, nullptr);
    const auto masterGuid = VNX::toFieldGroupGuid(projects.getMasterBusItem()->guid);
    auto groups = parent->fieldGroups;
    groups.mainOutput.shared = {VNX::FieldGroupMode::Local, VNX::MainOutputFieldGroup{masterGuid, -3.0f}};
    groups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        VNX::FieldGroupMode::Local, VNX::MainOutputFieldGroup{masterGuid, -9.0f}};
    groups.send.shared = {VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(sendBus->guid), -12.0f}}}};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        VNX::FieldGroupMode::Local, VNX::SendFieldGroup{}};
    groups.spatialRating.shared = {VNX::FieldGroupMode::Local, VNX::SpatialRatingFieldGroup{}};
    VNX::SpatialRatingFieldGroup android;
    android.spatialIs3DEnabled = true;
    android.spatialAttenuationDistanceScaling = 3.0f;
    groups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {VNX::FieldGroupMode::Local, android};
    ASSERT_TRUE(projects.restoreFieldGroupsInternal(parent, groups));
    expectSpatialized(false);
    EXPECT_FLOAT_EQ(projects.effectiveOutputVolumeDbForItem(sound), -3.0f);
    const auto sharedSends = projects.aggregateVoiceSendsForItem(sound);
    ASSERT_EQ(sharedSends.size(), 1u);
    EXPECT_EQ(sharedSends.front().targetBusID, sendBus->objectID);
    EXPECT_FLOAT_EQ(sharedSends.front().sendLevel, VNX::VMixNode::dbToLinear(-12.0f));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    const auto pending = VNX::AudioEngine::instance().playByNameDetailed(sound->name);
    EXPECT_EQ(pending.engineResult, HarkenEditor::Bridge::kEngineOk);
    EXPECT_NE(pending.playbackHandle, 0u);
    ASSERT_NO_FATAL_FAILURE(retirePlayback(pending));
    expectSpatialized(true, 3.0f);
    EXPECT_FLOAT_EQ(projects.effectiveOutputVolumeDbForItem(sound), -9.0f);
    EXPECT_TRUE(projects.aggregateVoiceSendsForItem(sound).empty());
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    expectSpatialized(false);
    EXPECT_FLOAT_EQ(projects.effectiveOutputVolumeDbForItem(sound), -3.0f);
    EXPECT_EQ(projects.aggregateVoiceSendsForItem(sound).size(), 1u);
}

TEST_F(FieldGroupNamePreview, InspectorAndNextPlaybackFollowAtomicEditsOwnershipAndResourceUndo)
{
    auto& projects = VNX::ProjectManager::instance();
    auto& actions = VNX::ActionManager::instance();
    VNX::registerSpatialActions(actions, nullptr, &projects);
    VNX::registerBuiltinInspectorCustomizations();
    auto* outputBus = projects.createItem(projects.getMasterBusItem(), QStringLiteral("Output"),
                                           VNX::ProjectItem::Type::Bus);
    auto* sendBus = projects.createItem(projects.getMasterBusItem(), QStringLiteral("Send"),
                                         VNX::ProjectItem::Type::Bus);
    ASSERT_NE(outputBus, nullptr);
    ASSERT_NE(sendBus, nullptr);
    const auto created = actions.execute("create_attenuation", {{"name", "InheritedDistance"}});
    ASSERT_TRUE(created.success) << created.errorMessage.toStdString();
    const QUuid attenuationGuid(created.data.value("guid").toString());
    ASSERT_FALSE(attenuationGuid.isNull());
    VNX::EffectiveDefinitionFieldGroups first;
    first.mainOutput = {VNX::toFieldGroupGuid(outputBus->guid), -3.0f};
    first.send.sends = {{VNX::toFieldGroupGuid(sendBus->guid), -8.0f}};
    first.spatialRating = {0.25f, true, VNX::toFieldGroupGuid(attenuationGuid), 2.0f};
    VNX::EffectiveDefinitionFieldGroups second;
    second.mainOutput = {VNX::toFieldGroupGuid(projects.getMasterBusItem()->guid), -9.0f};
    second.spatialRating = {-0.5f, false, {}, 3.0f};
    auto groups = parent->fieldGroups;
    groups.mainOutput.shared = {VNX::FieldGroupMode::Local, first.mainOutput};
    groups.send.shared = {VNX::FieldGroupMode::Local, first.send};
    groups.spatialRating.shared = {VNX::FieldGroupMode::Local, first.spatialRating};
    groups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {VNX::FieldGroupMode::Local, second.mainOutput};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {VNX::FieldGroupMode::Local, second.send};
    groups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {VNX::FieldGroupMode::Local, second.spatialRating};
    ASSERT_TRUE(projects.restoreFieldGroupsInternal(parent, groups));
    VNX::DefinitionFieldGroups otherGroups;
    otherGroups.mainOutput.shared = {VNX::FieldGroupMode::Local, second.mainOutput};
    otherGroups.send.shared = {VNX::FieldGroupMode::Local, second.send};
    otherGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, second.spatialRating};
    ASSERT_TRUE(projects.restoreFieldGroupsInternal(otherParent, otherGroups));
    auto dormant = sound->fieldGroups;
    dormant.mainOutput.shared.localHistory = first.mainOutput;
    dormant.send.shared.localHistory = first.send;
    dormant.spatialRating.shared.localHistory = first.spatialRating;
    ASSERT_TRUE(projects.restoreFieldGroupsInternal(sound, dormant));
    VNX::UndoRedoManager::instance().clear();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound);
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));

    QJsonObject edit{{"definition_guid", parent->guid.toString(QUuid::WithoutBraces)},
                     {"group", "mainOutput"}, {"scope", "Shared"},
                     {"field", "outputBusVolume"}, {"value", -6.0}};
    const auto changed = actions.execute("set_field_group_field", edit);
    ASSERT_TRUE(changed.success) << changed.errorMessage.toStdString();
    first.mainOutput.outputBusVolume = -6.0f;
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->undo();
    first.mainOutput.outputBusVolume = -3.0f;
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));
    undo->redo();
    first.mainOutput.outputBusVolume = -6.0f;
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));

    const auto before = parent->fieldGroups;
    const int undoCount = undo->count();
    const auto revision = VNX::AudioEngine::instance().runtimeSyncCoordinator().currentAuthoringRevision();
    edit.insert("field", "mainOutputBusRef");
    edit.insert("value", sendBus->guid.toString(QUuid::WithoutBraces));
    EXPECT_FALSE(actions.execute("set_field_group_field", edit).success);
    EXPECT_EQ(parent->fieldGroups, before);
    EXPECT_EQ(undo->count(), undoCount);
    EXPECT_EQ(VNX::AudioEngine::instance().runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));

    ASSERT_TRUE(projects.moveItem(sound, otherParent));
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, second));
    undo->undo();
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));
    auto androidExpected = second;
    androidExpected.mainOutput.outputBusVolume = -6.0f;
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, androidExpected));
    VNX::PlatformContextService::instance().setAll(true);
    EXPECT_EQ(VNX::PlatformContextService::instance().context().primary, HarkenTest::PlatformIds::Android);
    ASSERT_NO_FATAL_FAILURE(expectSpatialized(androidExpected.spatialRating.spatialIs3DEnabled,
        androidExpected.spatialRating.spatialAttenuationDistanceScaling));
    const auto allView = VNX::buildFieldGroupInspectorView(*projects.currentProject(), sound->guid,
        VNX::FieldGroupId::MainOutput, VNX::PlatformContextService::instance().context());
    ASSERT_TRUE(allView.valid);
    ASSERT_EQ(allView.perScope.size(), 2u);
    EXPECT_EQ(std::get<VNX::MainOutputFieldGroup>(allView.perScope.front().resolved.payload), first.mainOutput);
    EXPECT_EQ(std::get<VNX::MainOutputFieldGroup>(allView.perScope.back().resolved.payload), androidExpected.mainOutput);
    EXPECT_FALSE(allView.field(QStringLiteral("mainOutputBusRef")).uniform);
    EXPECT_FALSE(allView.fieldsEditable);
    auto* mixedRef = inspector.findChild<VNX::ObjectRefWidget*>("prop_mainOutputBusRef");
    ASSERT_NE(mixedRef, nullptr);
    EXPECT_EQ(mixedRef->currentObjectID(), 0u);
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));

    const auto outputGuid = first.mainOutput.mainOutputBusRef;
    ASSERT_TRUE(projects.deleteItem(outputBus));
    first.mainOutput.mainOutputBusRef = second.mainOutput.mainOutputBusRef;
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));
    EXPECT_EQ(sound->fieldGroups, dormant);
    undo->undo();
    first.mainOutput.mainOutputBusRef = outputGuid;
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));
    const auto sends = first.send;
    ASSERT_TRUE(projects.deleteItem(sendBus));
    first.send.sends.clear();
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));
    EXPECT_EQ(sound->fieldGroups, dormant);
    undo->undo();
    first.send = sends;
    ASSERT_NO_FATAL_FAILURE(expectGroups(inspector, first));
    EXPECT_EQ(sound->fieldGroups, dormant);
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
        for (int attempt = 0; attempt < 2048; ++attempt) {
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
        VNX::UndoRedoManager::instance().clear();
    }

    void expectGoodAction()
    {
        const auto result = engine.playByNameDetailed(QStringLiteral("Play_Good"));
        ASSERT_EQ(result.engineResult, HarkenEditor::Bridge::kEngineOk);
        ASSERT_NE(result.playbackHandle, 0u);
        EXPECT_GT(pumpUntilPreviewAudible(64), 0.05f);
    }

    void expectUnavailable(VNX::ProjectItem* item)
    {
        if (item->type == VNX::ProjectItem::Type::Action) {
            const auto result = engine.playByNameDetailed(item->name);
            EXPECT_NE(result.engineResult, HarkenEditor::Bridge::kEngineOk);
            EXPECT_EQ(result.playbackHandle, 0u);
        } else {
            const auto result = engine.playPreviewPlayable(kPreviewSound, item->runtimeId());
            EXPECT_NE(result.status, kPreviewOk);
            EXPECT_EQ(result.handle, 0u);
        }
    }

    std::vector<float> capture(uint32_t blocks)
    {
        auto* runtime = harken::detail::ActiveEngine();
        std::vector<float> output;
        for (uint32_t block = 0; block < blocks; ++block) {
            EXPECT_EQ(Vn_Update(), VN_OK);
            runtime->nullBackend.Pump(1);
            const float* samples = runtime->nullBackend.LastBlock();
            for (uint32_t frame = 0; frame < runtime->nullBackend.BlockFrames(); ++frame)
                output.push_back(samples[frame * runtime->nullBackend.Channels()]);
        }
        return output;
    }

    float livePeak(uint32_t blocks = 32)
    {
        auto* runtime = harken::detail::ActiveEngine();
        for (uint32_t block = 0; block < blocks; ++block) {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            runtime->nullBackend.Pump(1);
        }
        const auto count = runtime->nullBackend.BlockFrames() * runtime->nullBackend.Channels();
        const float* samples = runtime->nullBackend.LastBlock();
        float peak = 0;
        for (uint32_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(samples[i]));
        return peak;
    }
};

TEST_F(AudioPlaybackRegression, ActionAuditionSharesEmitterMemoryWithTypedPreview)
{
    auto* container = projects.createItem(layer, QStringLiteral("PerEmitter"),
                                          VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(projects.moveItem(sounds[0], container));
    ASSERT_TRUE(projects.moveItem(sounds[1], container));
    container->comp<VNX::VSelection>()->behavior.avoidRepeat = 1;
    auto* event = projects.createActionTargeting(container, VNX::VActionStep::Type::Play,
                                                QStringLiteral("Play_PerEmitter"));
    ASSERT_NE(event, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto direct = engine.playByNameDetailed(event->name);
    ASSERT_EQ(direct.engineResult, HarkenEditor::Bridge::kEngineOk);
    ASSERT_NE(direct.playbackHandle, 0u);
    EXPECT_GT(livePeak(), 0.05f);
    engine.stopPlayback(direct.playbackHandle, 0);
    EXPECT_LT(livePeak(64), 0.001f);

    HarkenEditor::AuditionManager audition(&engine);
    const auto first = audition.playItem(*event);
    ASSERT_NE(first, 0u);
    const float firstPeak = livePeak();
    ASSERT_GT(firstPeak, 0.05f);
    livePeak(1100);
    QMetaObject::invokeMethod(&audition, "pollAuditionStates", Qt::DirectConnection);
    EXPECT_EQ(harken::detail::ActiveEngine()->emitters.ActiveCount(), 1u);

    const auto second = audition.playItem(*event);
    ASSERT_NE(second, 0u);
    const float secondPeak = livePeak();
    ASSERT_GT(secondPeak, 0.05f);
    EXPECT_GT(std::abs(firstPeak - secondPeak), 0.05f);
    audition.stop(second, 0);
    livePeak(64);
    EXPECT_EQ(harken::detail::ActiveEngine()->emitters.ActiveCount(), 1u);

    const auto typed = engine.playPreviewPlayable(kPreviewContainer, container->runtimeId());
    ASSERT_EQ(typed.status, kPreviewOk);
    EXPECT_NEAR(livePeak(), firstPeak, 0.001f);
    EXPECT_EQ(engine.stopPreviewPlayable(typed.handle, 0), kPreviewOk);
    livePeak(64);
    EXPECT_EQ(engine.resetPreviewPlayable(kPreviewContainer, container->runtimeId()), kPreviewOk);
    livePeak(4);
    ASSERT_NE(audition.playItem(*event), 0u);
    EXPECT_GT(livePeak(), 0.05f);
    EXPECT_EQ(harken::detail::ActiveEngine()->emitters.ActiveCount(), 1u);
}

TEST_F(AudioPlaybackRegression, ActionAuditionEmitterRecoversAfterOutputRestart)
{
    auto* container = projects.createItem(layer, QStringLiteral("DefaultRandom"),
                                          VNX::ProjectItem::Type::Container);
    ASSERT_TRUE(projects.moveItem(sounds[0], container));
    auto* event = projects.createActionTargeting(container, VNX::VActionStep::Type::Play,
                                                QStringLiteral("Play_DefaultRandom"));
    ASSERT_NE(event, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    HarkenEditor::AuditionManager audition(&engine);
    ASSERT_NE(audition.playItem(*event), 0u);
    EXPECT_GT(livePeak(), 0.05f);
    const auto firstEpoch = engine.runtimeSyncCoordinator().installedWorld()->runtimeEpoch;
    audition.stopAll(0);
    livePeak(32);
    engine.stopOutput();
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_NE(engine.runtimeSyncCoordinator().installedWorld()->runtimeEpoch, firstEpoch);
    ASSERT_NE(audition.playItem(*event), 0u);
    EXPECT_GT(livePeak(), 0.05f);
    EXPECT_EQ(harken::detail::ActiveEngine()->emitters.ActiveCount(), 1u);
}

TEST_F(AudioPlaybackRegression, ActionAuditionPreservesEmitterExhaustionResult)
{
    std::vector<VnEmitterHandle> held;
    VnResult allocation = VN_OK;
    for (unsigned attempt = 0; attempt < 4096; ++attempt) {
        VnEmitterHandle emitter = 0;
        allocation = Vn_CreateEmitter(&emitter);
        if (allocation != VN_OK) break;
        held.push_back(emitter);
    }
    ASSERT_EQ(allocation, VN_NO_EMITTER);
    const auto refused = engine.playByNameWithPreviewContext(action->name);
    EXPECT_EQ(refused.playbackHandle, 0u);
    EXPECT_EQ(refused.engineResult, static_cast<HarkenEditor::Bridge::EngineResult>(VN_NO_EMITTER));
    for (const auto emitter : held) EXPECT_EQ(Vn_DestroyEmitter(emitter), VN_OK);
    const auto accepted = engine.playByNameWithPreviewContext(action->name);
    ASSERT_NE(accepted.playbackHandle, 0u);
    EXPECT_GT(livePeak(), 0.05f);
}

TEST_F(AudioPlaybackRegression, UnchangedMediaIsReusedWhenOnlyParametersChange)
{
    HarkenEditor::Bridge::CurrentWorldSoundPreparation preparation;
    ASSERT_TRUE(VNX::ProjectManager::prepareRuntimeSounds(*projects.currentProject(), preparation));
    ASSERT_EQ(preparation.sounds.size(), 2u);
    for (const auto& sound : preparation.sounds) EXPECT_TRUE(sound.samples.empty());
    const auto revisions = preparation.contentRevisions;
    sounds[0]->comp<VNX::VMixNode>()->volume = -12.0f;
    emit projects.itemPropertyChanged(sounds[0], QStringLiteral("volume"));
    ASSERT_TRUE(VNX::ProjectManager::prepareRuntimeSounds(*projects.currentProject(), preparation));
    EXPECT_EQ(preparation.contentRevisions, revisions);
    for (const auto& sound : preparation.sounds) EXPECT_TRUE(sound.samples.empty());
    sounds[0]->comp<VNX::VSound>()->sources.plain.entries.front().trimStartFrame = 128;
    ASSERT_TRUE(VNX::ProjectManager::prepareRuntimeSounds(*projects.currentProject(), preparation));
    for (const auto& sound : preparation.sounds)
        EXPECT_EQ(sound.samples.empty(), sound.binding.soundId != sounds[0]->runtimeId());
    EXPECT_NE(preparation.contentRevisions.at(sounds[0]->runtimeId()), revisions.at(sounds[0]->runtimeId()));
}

TEST_F(AudioPlaybackRegression, SpatialChangesReuseTheOriginalStereoPcm)
{
    std::vector<float> stereo(65536 * 2);
    for (size_t frame = 0; frame < stereo.size() / 2; ++frame) {
        stereo[frame * 2] = 0.125f;
        stereo[frame * 2 + 1] = -0.25f;
    }
    writeWavF32(paths[0], stereo, 2);
    auto& assets = VNX::AssetRegistry::instance();
    const auto* asset = assets.getAsset(930001);
    ASSERT_NE(asset, nullptr);
    auto metadata = asset->metadata;
    metadata.channels = 2;
    metadata.fileSize = static_cast<uint64_t>(QFileInfo(paths[0]).size());
    ASSERT_TRUE(assets.updateSourceFile(930001, metadata, paths[0],
        VNX::AssetImporter::computeContentHash(paths[0]), asset->lastModified));

    HarkenEditor::Bridge::CurrentWorldSoundPreparation preparation;
    ASSERT_TRUE(VNX::ProjectManager::prepareRuntimeSounds(
        *projects.currentProject(), preparation));
    const auto prepared = std::find_if(preparation.sounds.begin(), preparation.sounds.end(),
        [&](const auto& sound) { return sound.binding.soundId == sounds[0]->runtimeId(); });
    ASSERT_NE(prepared, preparation.sounds.end());
    ASSERT_EQ(prepared->channels, 2u);
    EXPECT_EQ(prepared->samples, stereo);
    ASSERT_EQ(HarkenEditor::Bridge::Detail::runtimeRegisterSoundPcm(
        prepared->binding.soundId, prepared->samples, prepared->channels,
        prepared->sampleRate, prepared->loopStartFrame, prepared->loopEndFrame), VN_OK);
    const auto revisions = preparation.contentRevisions;
    const auto expectReused = [&](bool spatialized) {
        ASSERT_TRUE(VNX::ProjectManager::prepareRuntimeSounds(
            *projects.currentProject(), preparation));
        EXPECT_EQ(preparation.contentRevisions, revisions);
        ASSERT_EQ(preparation.sounds.size(), 2u);
        for (const auto& sound : preparation.sounds) {
            EXPECT_TRUE(sound.samples.empty());
            if (sound.binding.soundId == sounds[0]->runtimeId())
                EXPECT_EQ(sound.binding.spatialized3D, spatialized);
        }
    };
    ASSERT_NO_FATAL_FAILURE(expectReused(false));

    VNX::SpatialRatingFieldGroup rating;
    rating.spatialIs3DEnabled = true;
    sounds[0]->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    auto* spatial = sounds[0]->comp<VNX::VSpatial>();
    ASSERT_NE(spatial, nullptr);
    for (float mix : {0.0f, 0.5f, 1.0f}) {
        SCOPED_TRACE(mix);
        spatial->spatializationMix = mix;
        spatial->spatializationMixPresent = true;
        ASSERT_NO_FATAL_FAILURE(expectReused(true));
    }
    rating.spatialIs3DEnabled = false;
    sounds[0]->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    ASSERT_NO_FATAL_FAILURE(expectReused(false));

    sounds[0]->comp<VNX::VSound>()->sources.plain.entries.front().trimStartFrame = 128;
    ASSERT_TRUE(VNX::ProjectManager::prepareRuntimeSounds(
        *projects.currentProject(), preparation));
    EXPECT_NE(preparation.contentRevisions.at(sounds[0]->runtimeId()),
              revisions.at(sounds[0]->runtimeId()));
}

TEST_F(AudioPlaybackRegression, InspectorVolumePreviewsCancelsCommitsAndUndoesWhilePlaying)
{
    ContainerActionReset actions(projects);
    VNX::registerBuiltinInspectorCustomizations();
    VNX::PlatformContextService::instance().setAll(true);
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sounds[1]);
    auto* slider = inspector.findChild<VNX::ParameterSlider*>("prop_volume");
    ASSERT_NE(slider, nullptr);
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const float initial = livePeak();
    ASSERT_GT(initial, 0.1f);
    const auto revision = engine.runtimeSyncCoordinator().currentAuthoringRevision();
    auto* binding = slider->binding();
    binding->beginEdit();
    binding->previewEdit(-12.0);
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.002f);
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    binding->cancelEdit();
    EXPECT_NEAR(livePeak(), initial, 0.002f);
    binding->beginEdit();
    binding->previewEdit(-6.0);
    binding->commitEdit();
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -6.0f / 20.0f), 0.002f);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_NEAR(livePeak(), initial, 0.002f);
}

TEST_F(AudioPlaybackRegression, BlueprintVolumeAndInheritedOutputGainChangeTheActiveVoice)
{
    ContainerActionReset actions(projects);
    VNX::PlatformContextService::instance().setAll(true);
    VNX::BlueprintCanvasWidget canvas;
    const auto originalFields = canvas.nodeContentFields();
    canvas.setNodeContentFields(VNX::kAllNodeContentFields);
    ASSERT_TRUE(canvas.focusItem(sounds[1]));
    VNX::NodeContentEditor* editor = nullptr;
    const auto& nodes = canvas.activeSignalPathSource()->graph().nodes;
    for (size_t index = 0; index < nodes.size(); ++index)
        if (nodes[index].key.objectGuid == sounds[1]->guid)
            editor = qobject_cast<VNX::NodeContentEditor*>(canvas.signalPathNode(index)->contentWidget());
    ASSERT_NE(editor, nullptr);
    auto* binding = editor->binding(VNX::NodeContentField::Volume);
    ASSERT_NE(binding, nullptr);
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const float initial = livePeak();
    ASSERT_GT(initial, 0.1f);
    binding->beginEdit();
    binding->previewEdit(-12.0);
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.002f);
    binding->cancelEdit();
    EXPECT_NEAR(livePeak(), initial, 0.002f);
    auto preview = layer->fieldGroups;
    preview.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(projects.getMasterBusItem()->guid), -12.0f}};
    emit projects.fieldGroupsPreviewed(layer, preview);
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.002f);
    emit projects.fieldGroupsPreviewed(layer, layer->fieldGroups);
    EXPECT_NEAR(livePeak(), initial, 0.002f);
    canvas.setNodeContentFields(originalFields);
}

TEST_F(AudioPlaybackRegression, VariableSweepsKeepBindingRevisionsStableAndPreserveLiveVolume)
{
    auto* variable = projects.createItem(nullptr, QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    auto* spec = variable->comp<VNX::VVariable>();
    spec->rangeMin = 0;
    spec->rangeMax = 1;
    spec->defaultValue = 0;
    auto binding = VNX::BindingSemantics::defaultBinding(VNX::BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = VNX::authoringUuid(HarkenTest::fixtureRuntimeStableId(*projects.currentProject()));
    binding.curve = {{0, 0}, {1, -12}};
    binding.handles.clear();
    binding.transitionMs = 0;
    ASSERT_TRUE(sounds[1]->comp<VNX::VMixNode>()->bindings.upsert(binding));
    emit projects.itemPropertyChanged(sounds[1], QStringLiteral("bindings"));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime->bindings.Find(harkentest::ToRuntime(binding.runtimeId())), nullptr);
    const auto revision = runtime->bindings.Find(harkentest::ToRuntime(binding.runtimeId()))->revision;
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const float initial = livePeak();
    ASSERT_GT(initial, 0.1f);
    for (int i = 0; i < 32; ++i) {
        const float value = static_cast<float>(i % 2);
        ASSERT_TRUE(projects.variableRehearsal().setOverride(variable->guid, value));
        const auto peak = livePeak(16);
        EXPECT_NEAR(peak, initial * std::pow(10.0f, -12.0f * value / 20.0f), 0.002f);
        EXPECT_EQ(runtime->bindings.Find(harkentest::ToRuntime(binding.runtimeId()))->revision, revision);
    }
    VNX::PropertyEditSession session(sounds[1], QStringLiteral("volume"), 0.0);
    session.preview(-6.0);
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -18.0f / 20.0f), 0.002f);
    session.cancel();
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.002f);
    engine.stopOutput();
    ASSERT_TRUE(engine.startOutput());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_NE(harken::detail::ActiveEngine()->bindings.Find(harkentest::ToRuntime(binding.runtimeId())), nullptr);
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.002f);
    ASSERT_TRUE(projects.variableRehearsal().setOverride(variable->guid, 0));
    EXPECT_NEAR(livePeak(16), initial, 0.002f);
}

TEST_F(AudioPlaybackRegression, ChangingRehearsalScopeResubmitsTheExistingValue)
{
    auto* variable = projects.createItem(nullptr, QStringLiteral("ScopedRehearsal"),
                                         VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    variable->comp<VNX::VVariable>()->rangeMax = 1;
    ASSERT_TRUE(VNX::BindingEditing::upsert(sounds[0],
        VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), variable->guid)));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[0]->runtimeId()).status, kPreviewOk);
    livePeak(2);
    const auto id = harkentest::ToRuntime(variable->runtimeId());
    VnEmitterHandle emitter = 0;
    harken::detail::ActiveEngine()->params.ForEachVariableTarget(id,
        [&](VnVoiceHandle, VnEmitterHandle owner, std::uint32_t) { emitter = owner; });
    ASSERT_NE(emitter, 0u);
    VnEmitterHandle other = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&other));
    auto& rehearsal = projects.variableRehearsal();
    ASSERT_TRUE(rehearsal.setOverride(variable->guid, 0.5f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(other, id, 0.75f));
    livePeak(16);
    const auto expectValue = [&](VnEmitterHandle handle, float expected, bool local) {
        VnVariableQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.variableId = id;
        query.emitter = handle;
        VnVariableQueryResultV1 value{};
        value.structSize = sizeof(value);
        ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &value));
        EXPECT_FLOAT_EQ(expected, value.currentValue);
        EXPECT_EQ(local, value.hasEmitterOverride != 0);
    };
    ASSERT_NO_FATAL_FAILURE(expectValue(0, 0, false));
    ASSERT_NO_FATAL_FAILURE(expectValue(emitter, 0.5f, true));
    ASSERT_NO_FATAL_FAILURE(expectValue(other, 0.75f, true));

    rehearsal.setScope(variable->guid, VNX::VariableScope::Global);
    livePeak(16);
    ASSERT_NO_FATAL_FAILURE(expectValue(0, 0.5f, false));
    ASSERT_NO_FATAL_FAILURE(expectValue(emitter, 0.5f, false));
    ASSERT_NO_FATAL_FAILURE(expectValue(other, 0.5f, false));

    rehearsal.setScope(variable->guid, VNX::VariableScope::Emitter);
    livePeak(16);
    ASSERT_NO_FATAL_FAILURE(expectValue(0, 0.5f, false));
    ASSERT_NO_FATAL_FAILURE(expectValue(emitter, 0.5f, true));
    ASSERT_NO_FATAL_FAILURE(expectValue(other, 0.5f, false));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(other));
}

TEST_F(AudioPlaybackRegression, VariableLayerAndBusUseTheirRuntimeEmitterContexts)
{
    auto* variable = projects.createItem(nullptr, QStringLiteral("X"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    auto* spec = variable->comp<VNX::VVariable>();
    spec->rangeMin = 1;
    spec->rangeMax = 3;
    spec->defaultValue = 1;
    auto* nested = projects.createItem(layer, QStringLiteral("Nested"), VNX::ProjectItem::Type::Layer);
    auto* other = projects.createItem(nullptr, QStringLiteral("Other"), VNX::ProjectItem::Type::Layer);
    auto* master = projects.getMasterBusItem();
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(other, nullptr);
    ASSERT_NE(master, nullptr);
    auto* bus = projects.createItem(master, QStringLiteral("OtherOutput"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    ASSERT_TRUE(projects.restoreFieldGroupsInternal(
        other, VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(bus->guid))));
    auto* nestedSound = projects.createAudioItem(nested, QStringLiteral("NestedSound"), paths[1], 930002);
    auto* otherSound = projects.createAudioItem(other, QStringLiteral("OtherSound"), paths[1], 930002);
    ASSERT_NE(nestedSound, nullptr);
    ASSERT_NE(otherSound, nullptr);
    auto* nestedAction = projects.createActionTargeting(
        nestedSound, VNX::VActionStep::Type::Play, QStringLiteral("PlayNested"));
    auto* otherAction = projects.createActionTargeting(
        otherSound, VNX::VActionStep::Type::Play, QStringLiteral("PlayOther"));
    ASSERT_NE(nestedAction, nullptr);
    ASSERT_NE(otherAction, nullptr);
    ASSERT_TRUE(::serviceUntilCurrentWorldReady(engine));
    for (auto* host : {layer, other, master, bus}) {
        auto binding = VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(host->type), variable->guid);
        binding.guid = QUuid::createUuid();
        binding.curve = {{0, 0}, {0.5f, -6}, {1, -12}};
        binding.handles.clear();
        binding.transitionMs = 0;
        ASSERT_TRUE(VNX::BindingEditing::upsert(host, binding));
    }
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    ASSERT_TRUE(::serviceUntilCurrentWorldReady(engine));
    livePeak(2);
    auto* runtime = harken::detail::ActiveEngine();
    for (auto* host : {layer, other, master, bus}) {
        const auto& binding = host->comp<VNX::VMixNode>()->bindings.entries().front();
        ASSERT_NE(runtime->bindings.Find(harkentest::ToRuntime(binding.runtimeId())), nullptr);
        unsigned attachmentCount = 0;
        runtime->bindings.ForEachAttachment([&](const auto& attachment, const auto&) {
            if (attachment.objectId == harkentest::ToRuntime(host->runtimeId()))
                ++attachmentCount;
        });
        ASSERT_EQ(1u, attachmentCount) << host->name.toStdString();
    }
    const auto id = harkentest::ToRuntime(variable->runtimeId());
    VnEmitterHandle a = 0;
    VnEmitterHandle b = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&a));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&b));
    const auto post = [&](VNX::ProjectItem* target, VnEmitterHandle emitter) {
        VnPostActionDesc desc{};
        desc.structSize = sizeof(desc);
        desc.actionId = harkentest::ToRuntime(target->runtimeId());
        desc.emitter = emitter;
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
        capture(4);
        VnActionStepResult step{};
        step.structSize = sizeof(step);
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
        ASSERT_EQ(VN_OK, step.result) << "state=" << step.state;
        ASSERT_NE(0u, step.producedPlayableInstance);
    };
    const auto expectInputsAndParameters = [&](float aValue, float bValue, float globalValue,
                                               unsigned aCount, unsigned aOverride) {
        unsigned foundA = 0;
        unsigned foundB = 0;
        runtime->params.ForEachVariableTarget(id,
            [&](VnVoiceHandle voice, VnEmitterHandle emitter, std::uint32_t target) {
                ASSERT_EQ(harken::kTargetVoiceVolume, target);
                ASSERT_TRUE(emitter == a || emitter == b);
                const float input = emitter == a ? aValue : bValue;
                const float db = -6.0f * (input - 1.0f);
                EXPECT_NEAR(db, runtime->params.ResolvedValue(voice, target), 1.0e-4f);
                const auto* state = runtime->voices.Resolve(voice);
                ASSERT_NE(state, nullptr);
                EXPECT_NEAR(std::pow(10.0f, db / 20.0f), state->gain.target, 1.0e-5f);
                emitter == a ? ++foundA : ++foundB;
            });
        EXPECT_EQ(aCount, foundA);
        EXPECT_EQ(1u, foundB);
        for (const auto emitter : {VnEmitterHandle{0}, a, b}) {
            VnVariableQueryDescV1 query{};
            query.structSize = sizeof(query);
            query.variableId = id;
            query.emitter = emitter;
            VnVariableQueryResultV1 value{};
            value.structSize = sizeof(value);
            ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &value));
            EXPECT_FLOAT_EQ(emitter == a ? aValue : emitter == b ? bValue : globalValue,
                            value.currentValue);
            EXPECT_EQ(emitter == a ? aOverride : 0u, value.hasEmitterOverride);
            EXPECT_EQ(0u, value.pendingCount);
        }
        for (auto* host : {master, bus}) {
            const auto index = runtime->buses.FindIndex(harkentest::ToRuntime(projects.busRuntimeHandle(host)));
            const auto* state = runtime->buses.BusState(index);
            ASSERT_NE(state, nullptr);
            EXPECT_NEAR(std::pow(10.0f, -6.0f * (globalValue - 1.0f) / 20.0f),
                        state->gain.target, 1.0e-5f);
        }
    };
    ASSERT_EQ(VN_OK, Vn_SetValue(id, 1));
    capture(16);
    ASSERT_NO_FATAL_FAILURE(post(nestedAction, a));
    ASSERT_NO_FATAL_FAILURE(post(nestedAction, b));
    ASSERT_NO_FATAL_FAILURE(post(otherAction, a));
    ASSERT_NO_FATAL_FAILURE(post(nestedAction, a));
    ASSERT_NO_FATAL_FAILURE(expectInputsAndParameters(1, 1, 1, 3, 0));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(a, id, 2));
    capture(16);
    ASSERT_NO_FATAL_FAILURE(expectInputsAndParameters(2, 1, 1, 3, 1));
    ASSERT_NO_FATAL_FAILURE(post(nestedAction, a));
    ASSERT_NO_FATAL_FAILURE(expectInputsAndParameters(2, 1, 1, 4, 1));
    ASSERT_EQ(VN_OK, Vn_SetValue(id, 3));
    capture(16);
    ASSERT_NO_FATAL_FAILURE(expectInputsAndParameters(3, 3, 3, 4, 0));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(a, id, 3));
    capture(16);
    ASSERT_NO_FATAL_FAILURE(expectInputsAndParameters(3, 3, 3, 4, 1));
    ASSERT_EQ(VN_OK, Vn_SetValue(id, 3));
    capture(16);
    ASSERT_NO_FATAL_FAILURE(expectInputsAndParameters(3, 3, 3, 4, 0));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(a));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(b));
}

TEST_F(AudioPlaybackRegression, InitialBindingVolumeValidationUsesThePlaybackEmitter)
{
    auto* variable = projects.createItem(nullptr, QStringLiteral("InitialVolume"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    variable->comp<VNX::VVariable>()->rangeMax = 1;
    ASSERT_TRUE(::serviceUntilCurrentWorldReady(engine));
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = QUuid::createUuid();
    binding.outputMin = 0;
    binding.outputMax = 800;
    binding.curve = {{0, 800}, {1, 0}};
    binding.handles.clear();
    binding.transitionMs = 0;
    ASSERT_TRUE(VNX::BindingEditing::upsert(layer, binding));
    const auto* authored = layer->comp<VNX::VMixNode>()->bindings.find(binding.runtimeId());
    ASSERT_NE(authored, nullptr);
    ASSERT_FLOAT_EQ(800, authored->curve.front().value);
    ASSERT_TRUE(::serviceUntilCurrentWorldReady(engine));
    livePeak(2);
    auto* runtime = harken::detail::ActiveEngine();
    const auto id = harkentest::ToRuntime(variable->runtimeId());
    VnEmitterHandle a = 0;
    VnEmitterHandle b = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&a));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&b));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(a, id, 1));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(b, id, 0));
    capture(16);

    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ToRuntime(action->runtimeId());
    desc.emitter = a;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    capture(4);
    VnActionStepResult step{};
    step.structSize = sizeof(step);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
    ASSERT_EQ(VN_OK, step.result);
    ASSERT_NE(0u, step.producedPlayableInstance);
    unsigned targets = 0;
    runtime->params.ForEachVariableTarget(id, [&](VnVoiceHandle voice, VnEmitterHandle emitter,
                                                std::uint32_t target) {
        EXPECT_EQ(a, emitter);
        EXPECT_FLOAT_EQ(0, runtime->params.ResolvedValue(voice, target));
        ++targets;
    });
    ASSERT_EQ(1u, targets);

    const auto liveBefore = runtime->playableInstances.LiveCount();
    const auto commandHead = runtime->commands.PublishedHead();
    for (const auto emitter : {VnEmitterHandle{0}, b}) {
        desc.emitter = emitter;
        VnActionFiringHandle refused = 0;
        EXPECT_EQ(VN_INVALID_ARG, Vn_PostAction(&desc, &refused));
        EXPECT_EQ(0u, refused);
        EXPECT_EQ(liveBefore, runtime->playableInstances.LiveCount());
        EXPECT_EQ(commandHead, runtime->commands.PublishedHead());
    }
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(a));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(b));
}

TEST_F(AudioPlaybackRegression, BindingCurveCommitAndUndoUpdateTheActiveVoiceAtFixedInput)
{
    ContainerActionReset actions(projects);
    VNX::registerBindingActions(VNX::ActionManager::instance());
    auto* variable = projects.createItem(nullptr, QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    auto* spec = variable->comp<VNX::VVariable>();
    spec->rangeMin = 0;
    spec->rangeMax = 1;
    spec->defaultValue = 1;
    auto* host = sounds[1];
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(host->type), variable->guid);
    binding.guid = QUuid::createUuid();
    binding.curve = {{0, 0}, {1, -12}};
    binding.handles = {{0, 0, 1, -12}};
    binding.transitionMs = 0;
    ASSERT_TRUE(VNX::BindingEditing::upsert(host, binding));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const float initial = livePeak();
    ASSERT_GT(initial, 0.01f);
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime->bindings.Find(harkentest::ToRuntime(binding.runtimeId())), nullptr);
    const auto revision = runtime->bindings.Find(harkentest::ToRuntime(binding.runtimeId()))->revision;
    const auto authoringRevision = engine.runtimeSyncCoordinator().currentAuthoringRevision();
    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::binding(host->guid, binding.address, binding.runtimeId()));
    ASSERT_TRUE(editor.canvas()->setPointCoordinates(1, 1, -24));
    EXPECT_GT(engine.runtimeSyncCoordinator().currentAuthoringRevision(), authoringRevision);
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.0005f);
    EXPECT_GT(runtime->bindings.Find(harkentest::ToRuntime(binding.runtimeId()))->revision, revision);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_NEAR(livePeak(), initial, 0.0005f);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.0005f);
}

static void sendCurveMouse(VNX::CurveCanvas& canvas, QEvent::Type type, const QPointF& pos,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, pos, QPointF(canvas.mapToGlobal(pos.toPoint())),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

TEST_F(AudioPlaybackRegression, BindingCurveDragPreviewsBeforeCommitAndRestoresOnCancel)
{
    ContainerActionReset actions(projects);
    VNX::registerBindingActions(VNX::ActionManager::instance());
    auto* variable = projects.createItem(nullptr, QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    auto* spec = variable->comp<VNX::VVariable>();
    spec->rangeMin = -10;
    spec->rangeMax = 10;
    spec->defaultValue = 0;
    auto* host = sounds[1];
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(host->type), variable->guid);
    binding.guid = QUuid::createUuid();
    binding.curve = {{0, 0}, {1, -12}};
    binding.handles = {{0, 0, 1, -12}};
    binding.transitionMs = 0;
    ASSERT_TRUE(VNX::BindingEditing::upsert(host, binding));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const float initial = livePeak();
    ASSERT_GT(initial, 0.05f);
    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::binding(host->guid, binding.address, binding.runtimeId()));
    auto& canvas = *editor.canvas();
    canvas.resize(800, 600);
    const auto original = host->comp<VNX::VMixNode>()->bindings.toJson();
    const auto revision = engine.runtimeSyncCoordinator().currentAuthoringRevision();
    const auto undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const float originalDb = VNX::curveBezierValueAt(canvas.curve(), canvas.curveHandles(),
        0, VNX::CurveAxisScale::MirroredAmplitudeDb);
    const auto expectPreview = [&] {
        const float db = VNX::curveBezierValueAt(canvas.curve(), canvas.curveHandles(),
            0, VNX::CurveAxisScale::MirroredAmplitudeDb);
        EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, (db - originalDb) / 20.0f), 0.0005f);
        EXPECT_EQ(host->comp<VNX::VMixNode>()->bindings.toJson(), original);
        EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(), revision);
        EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
    };
    const QPointF start = canvas.renderData().points.front().position;
    const QPointF moved = start + QPointF(0, 65);
    sendCurveMouse(canvas, QEvent::MouseButtonPress, start);
    sendCurveMouse(canvas, QEvent::MouseMove, moved);
    ASSERT_NE(canvas.curve().front().value, 0);
    expectPreview();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    EXPECT_NEAR(livePeak(), initial, 0.0005f);
    EXPECT_FLOAT_EQ(canvas.curve().front().value, 0);
    sendCurveMouse(canvas, QEvent::MouseButtonRelease, moved);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);

    sendCurveMouse(canvas, QEvent::MouseButtonPress, start, Qt::AltModifier);
    sendCurveMouse(canvas, QEvent::MouseMove, start + QPointF(150, 85), Qt::AltModifier);
    expectPreview();
    editor.clear();
    EXPECT_NEAR(livePeak(), initial, 0.0005f);
    EXPECT_EQ(host->comp<VNX::VMixNode>()->bindings.toJson(), original);

    editor.bindSelection(VNX::CurveSelection::binding(host->guid, binding.address, binding.runtimeId()));
    sendCurveMouse(canvas, QEvent::MouseButtonPress, start);
    sendCurveMouse(canvas, QEvent::MouseMove, moved);
    expectPreview();
    const float previewPeak = livePeak();
    sendCurveMouse(canvas, QEvent::MouseButtonRelease, moved);
    EXPECT_NEAR(livePeak(), previewPeak, 0.0005f);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_NEAR(livePeak(), initial, 0.0005f);
}

TEST_F(AudioPlaybackRegression, BindingCurveEditorsKeepIndependentPreviews)
{
    auto* variable = projects.createItem(nullptr, QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    auto* spec = variable->comp<VNX::VVariable>();
    spec->rangeMin = 0;
    spec->rangeMax = 1;
    spec->defaultValue = 0;
    auto* host = sounds[1];
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(host->type), variable->guid);
    binding.guid = QUuid::createUuid();
    binding.curve = {{0, 0}, {1, -12}};
    binding.handles = {{0, 0, 1, -12}};
    binding.transitionMs = 0;
    ASSERT_TRUE(VNX::BindingEditing::upsert(host, binding));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const float initial = livePeak();
    ASSERT_GT(initial, 0.1f);
    const auto original = host->comp<VNX::VMixNode>()->bindings.toJson();
    const auto revision = engine.runtimeSyncCoordinator().currentAuthoringRevision();
    const auto selection = VNX::CurveSelection::binding(host->guid, binding.address, binding.runtimeId());
    for (bool closeLatest : {false, true}) {
        auto first = std::make_unique<VNX::CurveEditorWidget>();
        auto second = std::make_unique<VNX::CurveEditorWidget>();
        first->bindSelection(selection);
        second->bindSelection(selection);
        const auto drag = [&](VNX::CurveEditorWidget& editor, double offset) {
            auto& canvas = *editor.canvas();
            canvas.resize(800, 600);
            const QPointF start = canvas.renderData().points.front().position;
            sendCurveMouse(canvas, QEvent::MouseButtonPress, start);
            sendCurveMouse(canvas, QEvent::MouseMove, start + QPointF(0, offset));
            const float expected = initial * std::pow(10.0f, canvas.curve().front().value / 20.0f);
            EXPECT_NEAR(livePeak(), expected, 0.0005f);
            return expected;
        };
        const float firstPeak = drag(*first, 65);
        const float secondPeak = drag(*second, 140);
        ASSERT_GT(std::abs(firstPeak - secondPeak), 0.01f);
        if (closeLatest) {
            second.reset();
            EXPECT_NEAR(livePeak(), firstPeak, 0.0005f);
            first->clear();
        } else {
            first.reset();
            EXPECT_NEAR(livePeak(), secondPeak, 0.0005f);
            second->canvas()->cancelDrag();
        }
        EXPECT_NEAR(livePeak(), initial, 0.0005f);
        EXPECT_EQ(host->comp<VNX::VMixNode>()->bindings.toJson(), original);
        EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    }
}

TEST_F(AudioPlaybackRegression, AttenuationCurvePreviewsCancelsCommitsAndUndoesWhilePlaying)
{
    ContainerActionReset actions(projects);
    VNX::registerSpatialActions(VNX::ActionManager::instance(), nullptr, &projects);
    VNX::AttenuationSnapshot attenuation;
    attenuation.name = QStringLiteral("Distance");
    attenuation.maxDistance = 100;
    auto& volume = attenuation.ensureDimension(VNX::AttenuationDimension::Volume);
    volume.curve = {{0, 0}, {100, 0}};
    volume.handles = {{0, 0, 100, 0}};
    auto* profile = VNX::AttenuationStore::create(projects.currentProject(), attenuation, &projects);
    ASSERT_NE(profile, nullptr);
    VNX::SpatialRatingFieldGroup spatial;
    spatial.spatialIs3DEnabled = true;
    spatial.spatialAttenuationRef = VNX::toFieldGroupGuid(profile->hostItem()->guid);
    auto groups = sounds[1]->fieldGroups;
    groups.spatialRating.shared = {VNX::FieldGroupMode::Local, spatial};
    ASSERT_TRUE(projects.restoreFieldGroupsInternal(sounds[1], groups));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const float initial = livePeak();
    ASSERT_GT(initial, 0.1f);
    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::attenuation(profile->hostItem()->guid,
        profile->hostItem()->name, VNX::AttenuationDimension::Volume));
    auto& canvas = *editor.canvas();
    canvas.resize(800, 600);
    const auto original = profile->toJson();
    const auto revision = engine.runtimeSyncCoordinator().currentAuthoringRevision();
    const auto undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const QPointF start = canvas.renderData().points.front().position;
    const QPointF moved = start + QPointF(0, 140);
    sendCurveMouse(canvas, QEvent::MouseButtonPress, start);
    sendCurveMouse(canvas, QEvent::MouseMove, moved);
    ASSERT_LT(canvas.curve().front().value, -1.0f);
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, canvas.curve().front().value / 20.0f), 0.0005f);
    EXPECT_EQ(profile->toJson(), original);
    EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
    canvas.cancelDrag();
    EXPECT_NEAR(livePeak(), initial, 0.0005f);
    sendCurveMouse(canvas, QEvent::MouseButtonRelease, moved);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
    ASSERT_TRUE(editor.canvas()->setPointCoordinates(0, 0, -12));
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.0005f);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_NEAR(livePeak(), initial, 0.0005f);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -12.0f / 20.0f), 0.0005f);
}

TEST_F(AudioPlaybackRegression, AttenuationCurveEditorsKeepIndependentPreviews)
{
    VNX::AttenuationSnapshot attenuation;
    attenuation.name = QStringLiteral("Distance");
    attenuation.maxDistance = 100;
    auto& volume = attenuation.ensureDimension(VNX::AttenuationDimension::Volume);
    volume.curve = {{0, 0}, {100, 0}};
    volume.handles = {{0, 0, 100, 0}};
    auto* profile = VNX::AttenuationStore::create(projects.currentProject(), attenuation, &projects);
    ASSERT_NE(profile, nullptr);
    VNX::SpatialRatingFieldGroup spatial;
    spatial.spatialIs3DEnabled = true;
    spatial.spatialAttenuationRef = VNX::toFieldGroupGuid(profile->hostItem()->guid);
    auto groups = sounds[1]->fieldGroups;
    groups.spatialRating.shared = {VNX::FieldGroupMode::Local, spatial};
    ASSERT_TRUE(projects.restoreFieldGroupsInternal(sounds[1], groups));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const float initial = livePeak();
    ASSERT_GT(initial, 0.1f);
    const auto original = profile->toJson();
    const auto revision = engine.runtimeSyncCoordinator().currentAuthoringRevision();
    const auto selection = VNX::CurveSelection::attenuation(profile->hostItem()->guid,
        profile->hostItem()->name, VNX::AttenuationDimension::Volume);
    for (bool closeLatest : {false, true}) {
        auto first = std::make_unique<VNX::CurveEditorWidget>();
        auto second = std::make_unique<VNX::CurveEditorWidget>();
        first->bindSelection(selection);
        second->bindSelection(selection);
        const auto drag = [&](VNX::CurveEditorWidget& editor, double offset) {
            auto& canvas = *editor.canvas();
            canvas.resize(800, 600);
            const QPointF start = canvas.renderData().points.front().position;
            sendCurveMouse(canvas, QEvent::MouseButtonPress, start);
            sendCurveMouse(canvas, QEvent::MouseMove, start + QPointF(0, offset));
            const float expected = initial * std::pow(10.0f, canvas.curve().front().value / 20.0f);
            EXPECT_NEAR(livePeak(), expected, 0.0005f);
            return expected;
        };
        const float firstPeak = drag(*first, 65);
        const float secondPeak = drag(*second, 140);
        ASSERT_GT(std::abs(firstPeak - secondPeak), 0.01f);
        if (closeLatest) {
            second.reset();
            EXPECT_NEAR(livePeak(), firstPeak, 0.0005f);
            first->clear();
        } else {
            first.reset();
            EXPECT_NEAR(livePeak(), secondPeak, 0.0005f);
            second->canvas()->cancelDrag();
        }
        EXPECT_NEAR(livePeak(), initial, 0.0005f);
        EXPECT_EQ(profile->toJson(), original);
        EXPECT_EQ(engine.runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    }
}

TEST_F(AudioPlaybackRegression, LivePitchTargetsThePreviewInstanceAndRestoresOnCancel)
{
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    ASSERT_GT(livePeak(), 0.1f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> paths;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(paths));
    ASSERT_EQ(paths.size(), 1u);
    auto* runtime = harken::detail::ActiveEngine();
    VNX::PropertyEditSession session(sounds[1], QStringLiteral("pitch"), 0.0);
    session.preview(1200.0);
    livePeak(4);
    EXPECT_NEAR(runtime->params.LastCommitted(paths.front().voiceInstance, harken::kTargetVoicePitch), 1200.0f, 0.01f);
    session.cancel();
    livePeak(4);
    EXPECT_NEAR(runtime->params.LastCommitted(paths.front().voiceInstance, harken::kTargetVoicePitch), 0.0f, 0.01f);
    EXPECT_EQ(Vn_SetVoicePitch(paths.front().voiceInstance, 2.0f), VN_STALE_HANDLE);
}

TEST_F(AudioPlaybackRegression, DuplicateUnicodeNamesKeepPlaybackAndLivePropertiesIndependent)
{
    const QString name = QStringLiteral("\u811a\u6b65");
    ASSERT_TRUE(projects.renameItem(sounds[0], name));
    ASSERT_TRUE(projects.renameItem(sounds[1], name));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto ambiguous = engine.playByNameDetailed(name);
    EXPECT_EQ(ambiguous.playbackHandle, 0u);
    EXPECT_EQ(ambiguous.engineResult, VN_INVALID_ARG);
    for (const auto* sound : sounds)
        ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sound->runtimeId()).status, kPreviewOk);
    ASSERT_GT(livePeak(), 0.1f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> observed;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(observed));
    ASSERT_EQ(observed.size(), 2u);
    auto* runtime = harken::detail::ActiveEngine();
    VNX::PropertyEditSession session(sounds[1], QStringLiteral("pitch"), 0.0);
    session.preview(1200.0);
    livePeak(4);
    for (const auto& path : observed) {
        EXPECT_TRUE(path.soundId == sounds[0]->runtimeId() || path.soundId == sounds[1]->runtimeId());
        EXPECT_NEAR(runtime->params.LastCommitted(path.voiceInstance, harken::kTargetVoicePitch),
                    path.soundId == sounds[1]->runtimeId() ? 1200.0f : 0.0f, 0.01f);
    }
    session.cancel();
    livePeak(4);
    for (const auto& path : observed)
        EXPECT_NEAR(runtime->params.LastCommitted(path.voiceInstance, harken::kTargetVoicePitch), 0.0f, 0.01f);
    ASSERT_TRUE(projects.renameItem(sounds[1], QStringLiteral("Other")));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto unique = engine.playByNameDetailed(name);
    EXPECT_EQ(unique.engineResult, HarkenEditor::Bridge::kEngineOk);
    EXPECT_NE(unique.playbackHandle, 0u);
}

TEST_F(AudioPlaybackRegression, FieldGroupPreviewPreservesVoiceWidth)
{
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    ASSERT_GT(livePeak(), 0.1f);
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> paths;
    ASSERT_TRUE(HarkenEditor::Bridge::capturePlaybackPaths(paths));
    ASSERT_EQ(paths.size(), 1u);
    auto* runtime = harken::detail::ActiveEngine();
    runtime->params.SetApiDirect(paths.front().voiceInstance, harken::kTargetVoiceWidth, 0.27f);
    livePeak(4);
    const auto original = sounds[1]->fieldGroups;
    auto preview = original;
    preview.spatialRating.shared = {VNX::FieldGroupMode::Local, VNX::SpatialRatingFieldGroup{}};
    preview.spatialRating.shared.localHistory->spatialPanning = 0.4f;
    emit projects.fieldGroupsPreviewed(sounds[1], preview);
    livePeak(4);
    EXPECT_EQ(sounds[1]->fieldGroups, original);
    EXPECT_NEAR(runtime->params.LastCommitted(paths.front().voiceInstance, harken::kTargetVoiceWidth), 0.27f, 0.001f);
    emit projects.fieldGroupsPreviewed(sounds[1], original);
    livePeak(4);
    EXPECT_NEAR(runtime->params.LastCommitted(paths.front().voiceInstance, harken::kTargetVoiceWidth), 0.27f, 0.001f);
}

TEST_F(AudioPlaybackRegression, InspectorInheritedOutputPreviewLeavesAuthoringUntouched)
{
    ContainerActionReset actions(projects);
    VNX::registerBuiltinInspectorCustomizations();
    VNX::PlatformContextService::instance().setAll(true);
    layer->fieldGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(projects.getMasterBusItem()->guid), 0.0f}};
    emit projects.itemPropertyChanged(layer, QStringLiteral("fieldGroups"));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto original = layer->fieldGroups;
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(layer);
    auto* slider = inspector.findChild<VNX::ParameterSlider*>("prop_outputBusVolume");
    ASSERT_NE(slider, nullptr);
    ASSERT_TRUE(slider->isEnabled());
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const auto initial = livePeak();
    ASSERT_GT(initial, 0.1f);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-18.0);
    EXPECT_EQ(layer->fieldGroups, original);
    EXPECT_NEAR(livePeak(), initial * std::pow(10.0f, -18.0f / 20.0f), 0.002f);
    slider->binding()->cancelEdit();
    EXPECT_NEAR(livePeak(), initial, 0.002f);
}

TEST_F(AudioPlaybackRegression, BlueprintSendGainPreviewsOnTheExistingRoute)
{
    ContainerActionReset actions(projects);
    VNX::PlatformContextService::instance().setAll(true);
    auto* sendBus = projects.createItem(projects.getMasterBusItem(), QStringLiteral("Send"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(sendBus, nullptr);
    sounds[1]->fieldGroups.send.shared = {VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(sendBus->guid), -6.0f}}}};
    emit projects.itemPropertyChanged(sounds[1], QStringLiteral("fieldGroups"));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto original = sounds[1]->fieldGroups;
    VNX::BlueprintCanvasWidget canvas;
    ASSERT_TRUE(canvas.focusItem(sounds[1]));
    const auto& edges = canvas.activeSignalPathSource()->graph().edges;
    VNX::NodeContentEditor* editor = nullptr;
    for (size_t index = 0; index < edges.size(); ++index)
        if (edges[index].kind == VNX::SignalPathEdgeKind::Send)
            editor = qobject_cast<VNX::NodeContentEditor*>(canvas.signalPathGainNode(index)->contentWidget());
    ASSERT_NE(editor, nullptr);
    auto* binding = editor->binding(VNX::NodeContentField::SendVolume);
    ASSERT_NE(binding, nullptr);
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    const auto initial = livePeak();
    ASSERT_GT(initial, 0.1f);
    binding->beginEdit();
    binding->previewEdit(-20.0);
    EXPECT_EQ(sounds[1]->fieldGroups, original);
    EXPECT_NEAR(livePeak(), initial * 1.1f / (1.0f + std::pow(10.0f, -6.0f / 20.0f)), 0.002f);
    binding->cancelEdit();
    EXPECT_NEAR(livePeak(), initial, 0.002f);
}

TEST_F(AudioPlaybackRegression, TrimmedSoundPlaysAfterReload)
{
    const QUuid trimmedGuid = sounds[1]->guid;
    auto& source = sounds[1]->comp<VNX::VSound>()->sources.plain.entries.front();
    source.trimStartFrame = 1790;
    source.trimEndFrame = 8192;
    const QString projectPath = projects.currentProject()->projectPath;
    ASSERT_TRUE(projects.saveProject());
    ASSERT_TRUE(projects.loadProject(projectPath));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    auto* trimmed = projects.findItemByGuid(trimmedGuid);
    ASSERT_NE(trimmed, nullptr);
    const auto pcm = harken::detail::ActiveEngine()->registry.Lookup(harkentest::ToRuntime(trimmed->runtimeId()));
    ASSERT_TRUE(pcm.Valid());
    EXPECT_EQ(pcm.frames, 8192u - 1790u);
    EXPECT_FLOAT_EQ(*pcm.SamplePtr(0, 0), 0.25f);
    const auto result = engine.playPreviewPlayable(kPreviewSound, trimmed->runtimeId());
    ASSERT_EQ(result.status, kPreviewOk);
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.1f);
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, SharedSourceKeepsPerSoundTrimAndOffset)
{
    std::vector<float> samples(8192, 0.125f);
    std::fill(samples.begin() + 4096, samples.end(), 0.5f);
    writeWavF32(paths[0], samples);
    auto* first = sounds[0]->comp<VNX::VSound>();
    first->sources.plain.entries.front().trimEndFrame = 2048;
    auto* second = sounds[1]->comp<VNX::VSound>();
    second->sources.plain.entries.front().sourceId = first->sources.plain.entries.front().sourceId;
    second->sources.plain.entries.front().trimStartFrame = 4096;
    second->sources.plain.entries.front().trimEndFrame = 6144;
    second->scheduleOffsetSec = 128.0 / 48000.0;
    VNX::ProjectManager::resyncItemToRuntime(sounds[0]);
    VNX::ProjectManager::resyncItemToRuntime(sounds[1]);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto firstPcm = harken::detail::ActiveEngine()->registry.Lookup(harkentest::ToRuntime(sounds[0]->runtimeId()));
    const auto secondPcm = harken::detail::ActiveEngine()->registry.Lookup(harkentest::ToRuntime(sounds[1]->runtimeId()));
    ASSERT_TRUE(firstPcm.Valid());
    ASSERT_TRUE(secondPcm.Valid());
    EXPECT_EQ(firstPcm.frames, 2048u);
    EXPECT_EQ(secondPcm.frames, 2176u);
    EXPECT_FLOAT_EQ(*firstPcm.SamplePtr(0, 0), 0.125f);
    for (uint64_t frame = 0; frame < 128; ++frame)
        EXPECT_FLOAT_EQ(*secondPcm.SamplePtr(0, frame), 0.0f);
    EXPECT_FLOAT_EQ(*secondPcm.SamplePtr(0, 128), 0.5f);
    EXPECT_EQ(HarkenEditor::Bridge::mediaRegistryFindPath(930001), paths[0]);
    const auto result = engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId());
    ASSERT_EQ(result.status, kPreviewOk);
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.3f);
}

TEST_F(AudioPlaybackRegression, LoopRegionPlaysIntroOnceThenRepeatsThenOutro)
{
    std::vector<float> samples(1536, 0.125f);
    std::fill(samples.begin() + 512, samples.begin() + 1024, 0.375f);
    std::fill(samples.begin() + 1024, samples.end(), 0.75f);
    writeWavF32(paths[1], samples);
    auto* sound = sounds[1]->comp<VNX::VSound>();
    sound->sources.plain.entries.front().loopRegion = VNX::SoundLoopRegion{512, 1024};
    ASSERT_TRUE(projects.applyItemPropertyLive(sounds[1], QStringLiteral("loopCount"), 3));
    VNX::ProjectManager::resyncItemToRuntime(sounds[1]);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto result = engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId());
    ASSERT_EQ(result.status, kPreviewOk);
    const auto output = capture(96);
    const auto countLevel = [&](float value) {
        return std::count_if(output.begin(), output.end(),
            [value](float sample) { return std::abs(sample - value) < 0.002f; });
    };
    EXPECT_NEAR(countLevel(0.125f), 512, 8);
    EXPECT_NEAR(countLevel(0.375f), 1536, 8);
    EXPECT_NEAR(countLevel(0.75f), 512, 8);
}

TEST_F(AudioPlaybackRegression, InfiniteLoopActionUsesRegionAndStops)
{
    std::vector<float> samples(1536, 0.125f);
    std::fill(samples.begin() + 512, samples.begin() + 1024, 0.375f);
    std::fill(samples.begin() + 1024, samples.end(), 0.75f);
    writeWavF32(paths[0], samples);
    sounds[0]->comp<VNX::VSound>()->sources.plain.entries.front().loopRegion = VNX::SoundLoopRegion{512, 1024};
    ASSERT_TRUE(projects.applyItemPropertyLive(sounds[0], QStringLiteral("loopCount"), 0));
    VNX::ProjectManager::resyncItemToRuntime(sounds[0]);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto result = engine.playByNameDetailed(action->name);
    ASSERT_EQ(result.engineResult, HarkenEditor::Bridge::kEngineOk);
    const auto output = capture(128);
    EXPECT_LT(*std::max_element(output.begin(), output.end()), 0.4f);
    EXPECT_NEAR(output.back(), 0.375f, 0.002f);
    engine.stopAll();
    const auto stopped = capture(16);
    EXPECT_NEAR(stopped.back(), 0.0f, 0.001f);
}

TEST_F(AudioPlaybackRegression, EmptySoundDoesNotBlockIndependentAction)
{
    auto* empty = projects.createItem(layer, QStringLiteral("EmptySound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(empty, nullptr);
    auto* unavailable = projects.createActionTargeting(empty, VNX::VActionStep::Type::Play,
                                                       QStringLiteral("Play_Empty"));
    ASSERT_NE(unavailable, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    expectUnavailable(empty);
    expectUnavailable(unavailable);
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, MissingAndCorruptMediaAreIsolatedAndRecover)
{
    auto* unavailable = projects.createActionTargeting(sounds[1], VNX::VActionStep::Type::Play,
                                                       QStringLiteral("Play_Missing"));
    ASSERT_NE(unavailable, nullptr);
    ASSERT_TRUE(QFile::remove(paths[1]));
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    expectUnavailable(sounds[1]);
    expectUnavailable(unavailable);
    expectGoodAction();
    QFile corrupt(paths[1]);
    ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("invalid audio");
    corrupt.close();
    VNX::ProjectManager::resyncItemToRuntime(sounds[1]);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    expectUnavailable(sounds[1]);
    expectGoodAction();
    writeWavF32(paths[1], std::vector<float>(65536, 0.5f));
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(930002, paths[1]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    EXPECT_EQ(engine.playByNameDetailed(unavailable->name).engineResult, HarkenEditor::Bridge::kEngineOk);
}

TEST_F(AudioPlaybackRegression, EmptyAndTargetlessActionsDoNotBlockPlayback)
{
    auto* empty = projects.createItem(nullptr, QStringLiteral("EmptyAction"), VNX::ProjectItem::Type::Action);
    auto* incomplete = projects.createItem(nullptr, QStringLiteral("IncompleteAction"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(incomplete, nullptr);
    incomplete->comp<VNX::VAction>()->steps.push_back(VNX::VActionStep{});
    emit projects.itemPropertyChanged(incomplete, QStringLiteral("steps"));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    expectUnavailable(empty);
    expectUnavailable(incomplete);
    expectGoodAction();
    QString error;
    EXPECT_FALSE(VNX::buildCurrentWorldSyncRequest(*projects.currentProject(), std::nullopt, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(AudioPlaybackRegression, IncompletePostActionChainDoesNotPartiallyExecute)
{
    auto* incomplete = projects.createItem(nullptr, QStringLiteral("Incomplete"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(incomplete, nullptr);
    auto* caller = projects.createActionTargeting(incomplete, VNX::VActionStep::Type::PostAction,
                                                 QStringLiteral("Caller"));
    ASSERT_NE(caller, nullptr);
    caller->comp<VNX::VAction>()->steps.push_back(action->comp<VNX::VAction>()->steps.front());
    auto* outer = projects.createActionTargeting(caller, VNX::VActionStep::Type::PostAction,
                                                QStringLiteral("Outer"));
    ASSERT_NE(outer, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    expectUnavailable(caller);
    expectUnavailable(outer);
    expectGoodAction();
    incomplete->comp<VNX::VAction>()->steps = action->comp<VNX::VAction>()->steps;
    emit projects.itemPropertyChanged(incomplete, QStringLiteral("steps"));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(engine.playByNameDetailed(outer->name).engineResult, HarkenEditor::Bridge::kEngineOk);
}

TEST_F(AudioPlaybackRegression, DuplicatedActionPlaysWithoutAnotherEdit)
{
    const auto revision = engine.runtimeSyncCoordinator().currentAuthoringRevision();
    auto* copy = projects.duplicateItem(action);
    ASSERT_NE(copy, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_GT(engine.runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    const auto result = engine.playByNameDetailed(copy->name);
    ASSERT_EQ(result.engineResult, HarkenEditor::Bridge::kEngineOk);
    ASSERT_NE(result.playbackHandle, 0u);
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.05f);
}

TEST_F(AudioPlaybackRegression, SamePathRefreshInstallsNewPcmWithoutManualResync)
{
    const auto previous = harken::detail::ActiveEngine()->registry.Lookup(harkentest::ToRuntime(sounds[1]->runtimeId())).contentRevision;
    writeWavF32(paths[1], std::vector<float>(65536, 0.5f));
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(930002, paths[1]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto pcm = harken::detail::ActiveEngine()->registry.Lookup(harkentest::ToRuntime(sounds[1]->runtimeId()));
    EXPECT_NE(pcm.contentRevision, previous);
    EXPECT_FLOAT_EQ(*pcm.SamplePtr(0, 0), 0.5f);
    const auto result = engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId());
    ASSERT_EQ(result.status, kPreviewOk);
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.3f);
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, SourceRefreshDuringPlaybackAllowsNewPlayback)
{
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    ASSERT_GT(pumpUntilPreviewAudible(64), 0.1f);
    writeWavF32(paths[1], std::vector<float>(32768, 0.5f));
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(930002, paths[1]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto pcm = harken::detail::ActiveEngine()->registry.Lookup(harkentest::ToRuntime(sounds[1]->runtimeId()));
    EXPECT_EQ(pcm.frames, 32768u);
    EXPECT_FLOAT_EQ(*pcm.SamplePtr(0, 0), 0.5f);
    EXPECT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, MultichannelTrimPreservesAllChannels)
{
    std::vector<float> samples(8192 * 6);
    for (size_t index = 0; index < samples.size(); ++index)
        samples[index] = 0.05f * static_cast<float>(index % 6 + 1);
    writeWavF32(paths[1], samples, 6);
    auto& source = sounds[1]->comp<VNX::VSound>()->sources.plain.entries.front();
    source.trimStartFrame = 512;
    source.trimEndFrame = 4096;
    VNX::ProjectManager::resyncItemToRuntime(sounds[1]);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto pcm = harken::detail::ActiveEngine()->registry.Lookup(harkentest::ToRuntime(sounds[1]->runtimeId()));
    ASSERT_TRUE(pcm.Valid());
    EXPECT_EQ(pcm.channelCount, 6u);
    EXPECT_EQ(pcm.frames, 3584u);
    for (uint32_t channel = 0; channel < 6; ++channel)
        EXPECT_FLOAT_EQ(*pcm.SamplePtr(channel, 0), 0.05f * (channel + 1));
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.05f);
}

TEST_F(AudioPlaybackRegression, WiderSpatialSourceRefusalKeepsRuntimeAndOtherSoundsUsable)
{
    std::vector<float> samples(8192 * 6);
    for (size_t frame = 0; frame < samples.size() / 6; ++frame)
        for (size_t channel = 0; channel < 6; ++channel)
            samples[frame * 6 + channel] = 0.025f * static_cast<float>(channel + 1);
    writeWavF32(paths[1], samples, 6);
    auto& source = sounds[1]->comp<VNX::VSound>()->sources.plain.entries.front();
    source.trimStartFrame = 512;
    source.trimEndFrame = 4096;
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialIs3DEnabled = true;
    sounds[1]->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    VNX::ProjectManager::resyncItemToRuntime(sounds[1]);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    ASSERT_FALSE(runtime->runtimeRecreateRequired);
    const auto pcm = runtime->registry.Lookup(harkentest::ToRuntime(sounds[1]->runtimeId()));
    ASSERT_TRUE(pcm.Valid());
    ASSERT_EQ(pcm.channelCount, 6u);
    ASSERT_EQ(pcm.frames, 3584u);
    ASSERT_NE(pcm.useCount, nullptr);
    for (uint32_t channel = 0; channel < 6; ++channel)
        EXPECT_FLOAT_EQ(*pcm.SamplePtr(channel, 0), 0.025f * (channel + 1));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto resources = [&] {
        std::uint32_t liveVoices = 0;
        runtime->ledger.ForEachLive([&](auto, auto, auto, auto) { ++liveVoices; });
        return std::array<std::uint32_t, 9>{
            runtime->voices.AvailableSlotCount(), runtime->voices.ActiveCount(), liveVoices,
            runtime->ledger.PendingStartCount(), runtime->ledger.ParkedCount(),
            runtime->ledger.QuarantinedCount(), runtime->playableInstances.LiveCount(),
            runtime->playableInstances.FreeCount(), runtime->playableInstances.QuarantinedCount()};
    };
    const auto before = resources();
    const auto refs = pcm.useCount->load(std::memory_order_acquire);
    const auto commandHead = runtime->commands.PublishedHead();
    for (int attempt = 0; attempt < 3; ++attempt) {
        SCOPED_TRACE(attempt);
        const auto refused = engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId(), emitter);
        EXPECT_EQ(HarkenEditor::Bridge::kPreviewInvalidArg, refused.status);
        EXPECT_EQ(0u, refused.handle);
        EXPECT_FALSE(runtime->runtimeRecreateRequired);
        EXPECT_EQ(before, resources());
        EXPECT_EQ(refs, pcm.useCount->load(std::memory_order_acquire));
        EXPECT_EQ(commandHead, runtime->commands.PublishedHead());
    }
    runtime->nullBackend.Pump(1);
    const float* rejected = runtime->nullBackend.LastBlock();
    const auto sampleCount = runtime->nullBackend.BlockFrames() * runtime->nullBackend.Channels();
    for (uint32_t index = 0; index < sampleCount; ++index)
        EXPECT_FLOAT_EQ(0.0f, rejected[index]);
    EXPECT_EQ(before, resources());

    const auto accepted = engine.playPreviewPlayable(kPreviewSound, sounds[0]->runtimeId(), emitter);
    ASSERT_EQ(kPreviewOk, accepted.status);
    ASSERT_NE(0u, accepted.handle);
    EXPECT_GT(pumpUntilPreviewAudible(8), 0.05f);
    EXPECT_FALSE(runtime->runtimeRecreateRequired);
    EXPECT_EQ(kPreviewOk, engine.stopPreviewPlayable(accepted.handle, 0));
    capture(8);
    EXPECT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
}

TEST_F(AudioPlaybackRegression, MissingContainerChildOnlyDisablesDependentPlayback)
{
    auto* container = projects.createItem(layer, QStringLiteral("Container"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(projects.moveItem(sounds[0], container));
    ASSERT_TRUE(projects.moveItem(sounds[1], container));
    auto* dependent = projects.createActionTargeting(container, VNX::VActionStep::Type::Play,
                                                     QStringLiteral("Play_Container"));
    ASSERT_NE(dependent, nullptr);
    ASSERT_TRUE(QFile::remove(paths[1]));
    VNX::ProjectManager::resyncItemToRuntime(container);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    expectUnavailable(dependent);
    const auto result = engine.playPreviewPlayable(kPreviewContainer, container->runtimeId());
    EXPECT_NE(result.status, kPreviewOk);
    EXPECT_EQ(result.handle, 0u);
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, RestoringIdenticalSourceContentRecoversPlayback)
{
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(930002, paths[1]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_TRUE(QFile::remove(paths[1]));
    VNX::ProjectManager::resyncItemToRuntime(sounds[1]);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    expectUnavailable(sounds[1]);
    writeWavF32(paths[1], std::vector<float>(65536, 0.25f));
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(930002, paths[1]));
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    ASSERT_EQ(engine.playPreviewPlayable(kPreviewSound, sounds[1]->runtimeId()).status, kPreviewOk);
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.1f);
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
    EXPECT_EQ(barrier.failure, HarkenEditor::Bridge::CurrentWorldSyncStatus::InvalidProject);
    EXPECT_EQ(runtime->variables.CurrentWorldGeneration(), generation);
    EXPECT_EQ(runtime->registry.Lookup(harkentest::ToRuntime(sounds[0]->runtimeId())).contentRevision, revision);
    EXPECT_TRUE(HarkenEditor::Bridge::runtimeObjectExists(sounds[1]->runtimeId()));

    action->comp<VNX::VAction>()->steps.front().type = VNX::VActionStep::Type::Play;
    coordinator.markCurrentWorldMutation();
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_NE(runtime->registry.Lookup(harkentest::ToRuntime(sounds[0]->runtimeId())).contentRevision, revision);
    expectUnavailable(sounds[1]);
    expectGoodAction();
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
    EXPECT_EQ(rejected.status, Bridge::CurrentWorldSyncStatus::CompileFailed);
    EXPECT_EQ(runtime->variables.CurrentWorldGeneration(), generation);
    EXPECT_EQ(runtime->registry.Lookup(harkentest::ToRuntime(sounds[0]->runtimeId())).contentRevision, revision);
    EXPECT_TRUE(Bridge::runtimeObjectExists(sounds[1]->runtimeId()));
    expectGoodAction();
}

TEST_F(AudioPlaybackRegression, GuardedLoopRefreshRetriesWithoutAnotherMutation)
{
    namespace Bridge = HarkenEditor::Bridge;
    auto* runtime = harken::detail::ActiveEngine();
    auto& source = sounds[1]->comp<VNX::VSound>()->sources.plain.entries.front();
    source.loopRegion = VNX::SoundLoopRegion{1024, 4096};
    VNX::ProjectManager::resyncItemToRuntime(sounds[1]);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    const auto previous = runtime->registry.Lookup(harkentest::ToRuntime(sounds[1]->runtimeId())).contentRevision;
    VnPlayDesc play{};
    play.structSize = sizeof(play);
    play.soundId = harkentest::ToRuntime(sounds[1]->runtimeId());
    play.busId = VN_BUS_MASTER;
    play.pitchRatio = 1.0f;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(harken::detail::PlayVoiceRaw(&play, &voice), VN_OK);
    const auto render = runtime->ledger.ToRender(voice);
    ASSERT_NE(render, 0u);
    harken::OccurrenceToken occurrence{};
    harken::OccurrenceKey key{};
    key.clipId = harkentest::ObjectId(81);
    ASSERT_TRUE(runtime->timelineOccurrences.Acquire(key, &occurrence));
    ASSERT_TRUE(runtime->timelineOccurrences.BindCompletionProgress(
        occurrence, static_cast<std::uint32_t>(render >> 32), 91));
    const auto owner = harken::CompletionOwnerRef::Single({81, 1}, 91);
    ASSERT_TRUE(runtime->timelineOccurrences.TryPinCompletionOwner(occurrence, owner));
    const harken::CompletionMutationBinding binding{occurrence, owner};
    ASSERT_TRUE(runtime->ledger.BindCompletionMutation(
        static_cast<std::uint32_t>(voice), binding));
    const std::vector<float> replacement(65536, 0.5f);
    ASSERT_EQ(Bridge::Detail::runtimeRegisterSoundPcm(
        harkentest::ToStudio(play.soundId), replacement, 1, 48000, 1024, 4096), VN_BACKPRESSURE);

    writeWavF32(paths[1], replacement);
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(930002, paths[1]));
    QCoreApplication::processEvents();
    auto& coordinator = engine.runtimeSyncCoordinator();
    coordinator.flushPendingMutationNow();
    const auto revision = coordinator.currentAuthoringRevision();
    EXPECT_EQ(coordinator.barrier(revision).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Pending);
    EXPECT_EQ(runtime->registry.Lookup(harkentest::ToRuntime(play.soundId)).contentRevision, previous);
    EXPECT_TRUE(Bridge::runtimeObjectExists(harkentest::ToStudio(play.soundId)));
    for (int attempt = 0; attempt < 3; ++attempt)
        QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
    EXPECT_EQ(coordinator.barrier(revision).state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Pending);

    ASSERT_TRUE(runtime->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(runtime->timelineOccurrences.RollbackCompletionOwnerPinExact(occurrence, owner));
    ASSERT_EQ(Vn_Stop(voice, 0), VN_OK);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    EXPECT_EQ(coordinator.currentAuthoringRevision(), revision);
    const auto pcm = runtime->registry.Lookup(harkentest::ToRuntime(play.soundId));
    EXPECT_NE(pcm.contentRevision, previous);
    EXPECT_FLOAT_EQ(*pcm.SamplePtr(0, 0), 0.5f);
    EXPECT_EQ(pcm.loop.startFrame, 1024u);
    EXPECT_EQ(pcm.LoopEndFrame(), 4096u);
    EXPECT_EQ(engine.playPreviewPlayable(kPreviewSound, harkentest::ToStudio(play.soundId)).status, kPreviewOk);
    EXPECT_GT(pumpUntilPreviewAudible(64), 0.3f);
}

TEST(AudioEnginePreview, NewActionPlayQueuesUntilProgramIsInstalled)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("queued-action.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("QueuedNewAction");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6995;
    asset.relativePath = QStringLiteral("queued-action.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = projects.createAudioItem(
        layer, QStringLiteral("QueuedActionSound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);
    auto* container = projects.createItem(layer, QStringLiteral("DefaultRandom"),
                                          VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(projects.moveItem(sound, container));
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    auto* action = projects.createActionTargeting(
        container, VNX::VActionStep::Type::Play, QStringLiteral("QueuedAction"));
    ASSERT_NE(action, nullptr);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    int32_t failureResult = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts, &failureResult](uint32_t, const QString&, int32_t result) {
            ++failedStarts;
            failureResult = result;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });

    const uint32_t playingId = audition.playItem(*action);
    ASSERT_NE(playingId, 0u);
    const auto waiting = audition.getAudition(playingId);
    EXPECT_FALSE(waiting.revisionPending);
    EXPECT_NE(audition.lastPlayEngineResult(),
              HarkenEditor::Bridge::kEngineUnknownId);
    EXPECT_NE(audition.lastPlayEngineResult(),
              HarkenEditor::Bridge::kEngineInvalidArg);
    if (waiting.actionInstallPending) {
        EXPECT_EQ(waiting.playbackHandle, 0u);
        EXPECT_EQ(successfulStarts, 0);
        EXPECT_EQ(failedStarts, 0);
        ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));
    }
    EXPECT_EQ(failedStarts, 0) << "engine result " << failureResult;
    EXPECT_NE(failureResult, static_cast<int32_t>(
                  HarkenEditor::Bridge::kEngineUnknownId));
    EXPECT_NE(failureResult, static_cast<int32_t>(
                  HarkenEditor::Bridge::kEngineInvalidArg));
    EXPECT_EQ(successfulStarts, 1);
    EXPECT_NE(audition.getAudition(playingId).playbackHandle, 0u);
    EXPECT_FALSE(audition.getAudition(playingId).actionInstallPending);
}

TEST(AudioEnginePreview, QueuedActionInstallFailureIsNotUnknownIdOrMissingWorld)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("failed-action.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("FailedQueuedAction");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6996;
    asset.relativePath = QStringLiteral("failed-action.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = projects.createAudioItem(
        layer, QStringLiteral("FailedActionSound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        backpressuredWorldInstall);
    auto* action = projects.createActionTargeting(
        sound, VNX::VActionStep::Type::Play, QStringLiteral("FailedQueuedAction"));
    ASSERT_NE(action, nullptr);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_FALSE(engine.hasActionProgram(action->runtimeId()));

    HarkenEditor::AuditionManager audition(&engine);
    int failedStarts = 0;
    int successfulStarts = 0;
    int32_t failureResult = 0;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStartFailed,
        [&failedStarts, &failureResult](uint32_t, const QString&, int32_t result) {
            ++failedStarts;
            failureResult = result;
        });
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionStarted,
        [&successfulStarts](uint32_t, const QString&) {
            ++successfulStarts;
        });

    const uint32_t playingId = audition.playItem(*action);
    ASSERT_NE(playingId, 0u);
    EXPECT_TRUE(audition.getAudition(playingId).actionInstallPending);

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        failedWorldInstall);
    auto barrier = engine.runtimeSyncCoordinator().barrier(
        engine.runtimeSyncCoordinator().currentAuthoringRevision());
    for (int attempt = 0;
         attempt < 128
         && barrier.state
             != VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed;
         ++attempt) {
        QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        barrier = engine.runtimeSyncCoordinator().barrier(
            engine.runtimeSyncCoordinator().currentAuthoringRevision());
    }
    EXPECT_EQ(barrier.state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);
    EXPECT_EQ(successfulStarts, 0);
    EXPECT_EQ(failedStarts, 1);
    EXPECT_EQ(failureResult, HarkenEditor::Bridge::kEngineNotInit);
    EXPECT_NE(failureResult, HarkenEditor::Bridge::kEngineUnknownId);
    EXPECT_NE(failureResult, 0);
    EXPECT_EQ(audition.getAudition(playingId).playingId, 0u);
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
}

TEST(AudioEnginePreview, FailedInstallActionPlayIsEngineNotInit)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString wavPath =
        QDir(directory.path()).filePath(QStringLiteral("failed-first-action.wav"));
    writeWavF32(wavPath, std::vector<float>(256, 0.25f));
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("FailedFirstActionPlay");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    VNX::AssetInfo asset;
    asset.sourceID = 6997;
    asset.relativePath = QStringLiteral("failed-first-action.wav");
    asset.absolutePath = wavPath;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    auto* layer = projects.createItem(
        nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = projects.createAudioItem(
        layer, QStringLiteral("FailedFirstActionSound"), wavPath, asset.sourceID);
    ASSERT_NE(sound, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        failedWorldInstall);
    auto* action = projects.createActionTargeting(
        sound, VNX::VActionStep::Type::Play, QStringLiteral("FailedFirstAction"));
    ASSERT_NE(action, nullptr);
    engine.runtimeSyncCoordinator().flushPendingMutationNow();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto barrier = engine.runtimeSyncCoordinator().barrier(
        engine.runtimeSyncCoordinator().currentAuthoringRevision());
    for (int attempt = 0;
         attempt < 128
         && barrier.state
             != VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed;
         ++attempt) {
        QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        barrier = engine.runtimeSyncCoordinator().barrier(
            engine.runtimeSyncCoordinator().currentAuthoringRevision());
    }
    EXPECT_EQ(barrier.state,
              VNX::RuntimeSyncCoordinator::RevisionBarrierState::Failed);
    EXPECT_FALSE(engine.hasActionProgram(action->runtimeId()));

    HarkenEditor::AuditionManager audition(&engine);
    EXPECT_EQ(audition.playItem(*action), 0u);
    EXPECT_EQ(audition.lastPlayFailure(),
              HarkenEditor::Bridge::PlaybackFailure::RuntimeRefused);
    EXPECT_EQ(audition.lastPlayEngineResult(),
              HarkenEditor::Bridge::kEngineNotInit);
    EXPECT_NE(audition.lastPlayFailure(),
              HarkenEditor::Bridge::PlaybackFailure::NotPlayable);
    EXPECT_NE(audition.lastPlayEngineResult(),
              HarkenEditor::Bridge::kEngineUnknownId);
    const auto snapshot = audition.previewIpcSnapshot();
    EXPECT_NE(snapshot.value(QStringLiteral("engine_result")).toInt(), -200);
    EXPECT_NE(snapshot.value(QStringLiteral("engine_result")).toInt(), 0);
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
}

TEST(AudioEnginePreview, EmptyActionPreviewIsStudioRefusalWithoutEngineCode)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("EmptyActionPreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    auto* action = projects.createItem(
        nullptr, QStringLiteral("EmptyAction"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    const auto direct = engine.playByNameDetailed(action->name);
    EXPECT_EQ(direct.playbackHandle, 0u);
    EXPECT_EQ(direct.engineResult, HarkenEditor::Bridge::kEngineUnknownId);

    HarkenEditor::AuditionManager audition(&engine);
    EXPECT_EQ(audition.playItem(*action), 0u);
    EXPECT_EQ(audition.lastPlayFailure(),
              HarkenEditor::Bridge::PlaybackFailure::NotPlayable);
    EXPECT_EQ(audition.lastPlayEngineResult(), HarkenEditor::Bridge::kEngineOk);
    EXPECT_EQ(audition.lastPlayReason(), QStringLiteral("empty"));
    const auto snapshot = audition.previewIpcSnapshot();
    EXPECT_EQ(snapshot.value(QStringLiteral("engine_result")).toInt(), 0);
    EXPECT_NE(snapshot.value(QStringLiteral("engine_result")).toInt(), -200);
}

TEST(AudioEnginePreview, TargetlessPlayPreviewIsStudioRefusalWithoutEngineCode)
{
    ensureApp();
    AudioEnginePreviewReset reset;
    CurrentWorldSyncStateReset syncReset;
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto& projects = VNX::ProjectManager::instance();
    VNX::AssetRegistry::instance().clear();

    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("TargetlessActionPreview");
    projectConfig.location = directory.path();
    ASSERT_TRUE(projects.createProject(projectConfig));
    auto* action = projects.createItem(
        nullptr, QStringLiteral("TargetlessAction"),
        VNX::ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    VNX::VActionStep step;
    step.type = VNX::VActionStep::Type::Play;
    action->comp<VNX::VAction>()->steps.push_back(step);
    VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    EXPECT_EQ(audition.playItem(*action), 0u);
    EXPECT_EQ(audition.lastPlayFailure(),
              HarkenEditor::Bridge::PlaybackFailure::NotPlayable);
    EXPECT_EQ(audition.lastPlayEngineResult(), HarkenEditor::Bridge::kEngineOk);
    EXPECT_EQ(audition.lastPlayReason(), QStringLiteral("empty"));
    const auto snapshot = audition.previewIpcSnapshot();
    EXPECT_EQ(snapshot.value(QStringLiteral("engine_result")).toInt(), 0);
    EXPECT_NE(snapshot.value(QStringLiteral("engine_result")).toInt(), -200);
}

TEST_F(AudioPlaybackRegression, EmptySoundActionPreviewIsStudioRefusalWithoutEngineCode)
{
    auto* empty = projects.createItem(
        layer, QStringLiteral("EmptySoundPreview"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(empty, nullptr);
    auto* unavailable = projects.createActionTargeting(
        empty, VNX::VActionStep::Type::Play, QStringLiteral("Play_EmptyPreview"));
    ASSERT_NE(unavailable, nullptr);
    ASSERT_TRUE(serviceUntilCurrentWorldReady(engine));

    HarkenEditor::AuditionManager audition(&engine);
    EXPECT_EQ(audition.playItem(*unavailable), 0u);
    EXPECT_EQ(audition.lastPlayFailure(),
              HarkenEditor::Bridge::PlaybackFailure::NotPlayable);
    EXPECT_EQ(audition.lastPlayEngineResult(), HarkenEditor::Bridge::kEngineOk);
    EXPECT_NE(audition.lastPlayReason(), QStringLiteral("empty"));
    const auto snapshot = audition.previewIpcSnapshot();
    EXPECT_EQ(snapshot.value(QStringLiteral("engine_result")).toInt(), 0);
    EXPECT_NE(snapshot.value(QStringLiteral("engine_result")).toInt(), -200);
    expectUnavailable(unavailable);
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

    void serviceBlocks(int count)
    {
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        for (int block = 0; block < count; ++block) {
            ASSERT_TRUE(QMetaObject::invokeMethod(
                &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
            runtime->nullBackend.Pump(1);
            ASSERT_TRUE(QMetaObject::invokeMethod(
                &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        }
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
    EXPECT_FALSE(accepted.data.value(QStringLiteral("native_released")).toBool());

    VNX::ActionResult applied;
    ASSERT_TRUE(waitForSeekTerminal(operation, applied));
    ASSERT_TRUE(applied.success) << applied.errorMessage.toStdString();
    EXPECT_EQ(applied.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("applied"));
    EXPECT_TRUE(applied.data.value(QStringLiteral("native_released")).toBool());
    EXPECT_EQ(applied.data.value(QStringLiteral("target_count")).toInt(), 1);
    EXPECT_EQ(applied.data.value(QStringLiteral("applied_count")).toInt(), 1);
    EXPECT_EQ(applied.data.value(
                  QStringLiteral("minimum_applied_source_frame")).toInteger(),
              1024);
    EXPECT_EQ(applied.data.value(
                  QStringLiteral("maximum_applied_source_frame")).toInteger(),
              1024);

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

    const auto relative = seek(second, 128.0 * frameMs, true);
    ASSERT_TRUE(relative.success) << relative.errorMessage.toStdString();
    const uint64_t relativeOperation = operationId(relative);
    VNX::ActionResult relativeApplied;
    ASSERT_TRUE(waitForSeekTerminal(relativeOperation, relativeApplied));
    ASSERT_TRUE(relativeApplied.success)
        << relativeApplied.errorMessage.toStdString();
    EXPECT_EQ(relativeApplied.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("applied"));
    EXPECT_GT(relativeApplied.data.value(
                  QStringLiteral("minimum_applied_source_frame")).toInteger(),
              128);
    EXPECT_TRUE(release(relativeOperation).success);
}

TEST_F(StudioActionSeekTest, RejectsInvalidReferenceTargetAndUnsupportedShapes)
{
    const auto marker = VNX::ActionManager::instance().execute(
        QStringLiteral("seek_action"),
        QJsonObject{{QStringLiteral("snap_to_marker"), true}},
        VNX::ActionSource::IPC);
    EXPECT_FALSE(marker.success);

    for (const auto& [key, value] : {
             std::pair{QStringLiteral("snap_to_marker"), QJsonValue(1)},
             std::pair{QStringLiteral("is_relative"),
                       QJsonValue(QStringLiteral("true"))}}) {
        const auto invalidBool = VNX::ActionManager::instance().execute(
            QStringLiteral("seek_action"),
            QJsonObject{
                {QStringLiteral("action_handle"), QStringLiteral("1")},
                {QStringLiteral("position"), 0.0},
                {key, value},
            },
            VNX::ActionSource::IPC);
        EXPECT_FALSE(invalidBool.success);
    }

    const auto missingHandle = VNX::ActionManager::instance().execute(
        QStringLiteral("seek_action"),
        QJsonObject{{QStringLiteral("position"), 1.0}},
        VNX::ActionSource::IPC);
    EXPECT_FALSE(missingHandle.success);

    const quintptr handle = postAction();
    std::vector<VnVoiceHandle> voices;
    ASSERT_TRUE(waitForActionVoices(1, voices));
    const auto mismatched = seek(
        handle, 10.0, false, QStringLiteral("OtherAction"));
    EXPECT_FALSE(mismatched.success);
    EXPECT_EQ(mismatched.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("rejected"));

    const auto negative = seek(handle, -1.0);
    EXPECT_FALSE(negative.success);
    const auto outOfRange = seek(handle, 1000000.0);
    EXPECT_FALSE(outOfRange.success);
    const auto nan = engine.seekAction(
        handle, action->name, std::numeric_limits<double>::quiet_NaN(), false);
    EXPECT_EQ(nan.result, HarkenEditor::Bridge::kEngineInvalidArg);
    EXPECT_EQ(nan.operationId, 0u);

    const auto preview = engine.playPreviewPlayable(
        kPreviewSound, sound->runtimeId());
    ASSERT_EQ(preview.status, kPreviewOk);
    ASSERT_NE(preview.handle, 0u);
    const auto wrongTarget = seek(
        static_cast<quintptr>(preview.handle), 10.0);
    EXPECT_FALSE(wrongTarget.success);

    const auto rawSound = engine.mintRawPcmSoundId();
    const auto rawRegistration = engine.registerRawPcmSlice(
        rawSound, wavPath, 0, 4096, false);
    ASSERT_EQ(rawRegistration.result, HarkenEditor::Bridge::kEngineOk);
    const uint64_t rawPlayback = engine.playRawPcm(rawSound, false);
    ASSERT_NE(rawPlayback, 0u);
    const auto rawTarget = seek(static_cast<quintptr>(rawPlayback), 10.0);
    EXPECT_FALSE(rawTarget.success);
    EXPECT_EQ(rawTarget.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("rejected"));
    EXPECT_TRUE(engine.stopRawPcm(rawPlayback));

    engine.stopPlayback(handle, 0.0f);
    serviceBlocks(4);
    const auto ended = seek(handle, 10.0);
    EXPECT_FALSE(ended.success);

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

TEST_F(StudioActionSeekTest,
       CompletedButUnreleasedOperationsRetainBoundedStudioCapacity)
{
    const quintptr playback = postAction();
    std::vector<VnVoiceHandle> voices;
    ASSERT_TRUE(waitForActionVoices(1, voices));
    std::vector<uint64_t> operations;
    operations.reserve(256);
    const double positionMs = 1024.0 * 1000.0 / 48000.0;
    for (int index = 0; index < 256; ++index) {
        const auto started = seek(playback, positionMs);
        ASSERT_TRUE(started.success)
            << index << ": " << started.errorMessage.toStdString();
        const uint64_t operation = operationId(started);
        ASSERT_NE(operation, 0u);
        VNX::ActionResult completed;
        ASSERT_TRUE(waitForSeekTerminal(operation, completed));
        ASSERT_TRUE(completed.success)
            << index << ": " << completed.errorMessage.toStdString();
        ASSERT_EQ(completed.data.value(QStringLiteral("state")).toString(),
                  QStringLiteral("applied"));
        ASSERT_TRUE(completed.data.value(
            QStringLiteral("native_released")).toBool());
        operations.push_back(operation);
    }

    const auto full = seek(playback, positionMs);
    EXPECT_FALSE(full.success);
    EXPECT_EQ(operationId(full), 0u);

    for (const uint64_t operation : operations)
        ASSERT_TRUE(release(operation).success);
    const auto afterRelease = seek(playback, positionMs);
    ASSERT_TRUE(afterRelease.success)
        << afterRelease.errorMessage.toStdString();
    const uint64_t afterReleaseOperation = operationId(afterRelease);
    VNX::ActionResult afterReleaseTerminal;
    ASSERT_TRUE(waitForSeekTerminal(
        afterReleaseOperation, afterReleaseTerminal));
    EXPECT_TRUE(afterReleaseTerminal.success);
    EXPECT_TRUE(release(afterReleaseOperation).success);
}

TEST_F(StudioActionSeekTest, AuditionPositionMovesOnlyAfterApplied)
{
    HarkenEditor::AuditionManager audition(&engine);
    VNX::ActionManager::instance().clearAllForTesting();
    VNX::registerAuditionActions(
        VNX::ActionManager::instance(), &audition, &engine);
    const uint32_t playingId = audition.playItem(*action);
    ASSERT_NE(playingId, 0u);
    std::vector<VnVoiceHandle> voices;
    ASSERT_TRUE(waitForActionVoices(1, voices));
    ASSERT_NE(audition.getAudition(playingId).playbackHandle, 0u);

    int positionSignals = 0;
    int finishedSignals = 0;
    quint64 finishedRequestId = 0;
    bool applied = false;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionPositionChanged,
        &audition, [&](uint32_t id, double) {
            if (id == playingId)
                ++positionSignals;
        });
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

    const auto accepted = VNX::ActionManager::instance().execute(
        QStringLiteral("audition_set_position"),
        QJsonObject{{QStringLiteral("auditionID"),
                     static_cast<int>(playingId)},
                    {QStringLiteral("samplePosition"), 1024.0}},
        VNX::ActionSource::IPC);
    ASSERT_TRUE(accepted.success) << accepted.errorMessage.toStdString();
    EXPECT_EQ(accepted.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("accepted"));
    const uint64_t requestId = accepted.data.value(
        QStringLiteral("audition_seek_request_id")).toString().toULongLong();
    EXPECT_NE(requestId, 0u);
    EXPECT_EQ(positionSignals, 0);
    EXPECT_EQ(finishedSignals, 0);

    for (int attempt = 0; attempt < 128 && finishedSignals == 0; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "pollAuditionStates", Qt::DirectConnection));
        QThread::yieldCurrentThread();
    }
    EXPECT_EQ(positionSignals, 1);
    EXPECT_EQ(finishedSignals, 1);
    EXPECT_EQ(finishedRequestId, requestId);
    EXPECT_TRUE(applied);

    const auto rejected = VNX::ActionManager::instance().execute(
        QStringLiteral("audition_set_position"),
        QJsonObject{{QStringLiteral("auditionID"),
                     static_cast<int>(playingId)},
                    {QStringLiteral("samplePosition"), -1.0}},
        VNX::ActionSource::IPC);
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.data.value(QStringLiteral("state")).toString(),
              QStringLiteral("rejected"));
    EXPECT_EQ(positionSignals, 1);
    EXPECT_EQ(finishedSignals, 1);

    for (const QJsonObject& invalid : {
             QJsonObject{{QStringLiteral("auditionID"), 1.5},
                         {QStringLiteral("samplePosition"), 0.0}},
             QJsonObject{{QStringLiteral("auditionID"),
                          static_cast<int>(playingId)},
                         {QStringLiteral("samplePosition"), 1.5}},
             QJsonObject{{QStringLiteral("auditionID"),
                          static_cast<int>(playingId)}}}) {
        const auto invalidResult = VNX::ActionManager::instance().execute(
            QStringLiteral("audition_set_position"), invalid,
            VNX::ActionSource::IPC);
        EXPECT_FALSE(invalidResult.success);
    }
}

TEST_F(StudioActionSeekTest,
       AuditionSeekUsesExactHandleWithoutDisplayNameCrossCheck)
{
    HarkenEditor::AuditionManager audition(&engine);
    const quintptr playback = postAction();
    std::vector<VnVoiceHandle> voices;
    ASSERT_TRUE(waitForActionVoices(1, voices));
    const uint32_t playingId = audition.registerExternalPlayback(
        QStringLiteral("Actions/DisplayAlias"), playback);
    ASSERT_NE(playingId, 0u);

    int finishedSignals = 0;
    quint64 finishedRequestId = 0;
    int32_t finishedResult = HarkenEditor::Bridge::kEngineNotInit;
    bool applied = false;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionSeekFinished,
        &audition, [&](uint32_t id, quint64 requestId, int32_t result,
                       bool didApply) {
            if (id == playingId) {
                ++finishedSignals;
                finishedRequestId = requestId;
                finishedResult = result;
                applied = didApply;
            }
        });

    const auto started = audition.setPositionSamples(playingId, 1024);
    ASSERT_EQ(started.result, HarkenEditor::Bridge::kEngineOk);
    ASSERT_NE(started.requestId, 0u);
    for (int attempt = 0; attempt < 128 && finishedSignals == 0; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &audition, "pollAuditionStates", Qt::DirectConnection));
        QThread::yieldCurrentThread();
    }
    EXPECT_EQ(finishedSignals, 1);
    EXPECT_EQ(finishedRequestId, started.requestId);
    EXPECT_EQ(finishedResult, HarkenEditor::Bridge::kEngineOk);
    EXPECT_TRUE(applied);
    EXPECT_EQ(audition.getPositionSamples(playingId).samplePosition, 1024);
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
    int32_t finishedResult = 0;
    bool applied = true;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionSeekFinished,
        &audition, [&](uint32_t id, quint64, int32_t result,
                       bool didApply) {
            if (id == playingId) {
                ++finishedSignals;
                finishedResult = result;
                applied = didApply;
            }
        });
    const auto started = audition.setPositionSamples(playingId, 1024);
    ASSERT_EQ(started.result, HarkenEditor::Bridge::kEngineOk);

    engine.shutdown();
    EXPECT_EQ(finishedSignals, 1);
    EXPECT_EQ(finishedResult, HarkenEditor::Bridge::kEngineNotInit);
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
    int32_t finishedResult = HarkenEditor::Bridge::kEngineOk;
    bool applied = true;
    QObject::connect(
        &audition, &HarkenEditor::AuditionManager::auditionSeekFinished,
        &audition, [&](uint32_t id, quint64 requestId, int32_t result,
                       bool didApply) {
            if (id == playingId) {
                ++finishedSignals;
                finishedRequestId = requestId;
                finishedResult = result;
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
    EXPECT_EQ(finishedResult, HarkenEditor::Bridge::kEngineNotInit);
    EXPECT_FALSE(applied);
    EXPECT_EQ(audition.getPositionSamples(playingId).samplePosition, 0);

    ASSERT_TRUE(QMetaObject::invokeMethod(
        &audition, "pollAuditionStates", Qt::DirectConnection));
    EXPECT_EQ(finishedSignals, 1);
}

}
