#include "Engine.h"
#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Widgets/Monitor/MonitorPanel.h"
#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Components/VAction.h"
#include "Components/VVariable.h"
#include "Core/AudioEngine.h"
#include "Core/ProjectManager.h"
#include "Core/RuntimeSyncCoordinator.h"
#include "Core/UndoCommands.h"
#include "Core/WorkspaceWindowManager.h"
#include "MainWindow.h"
#include "RuntimeBridge/PreviewBridge.h"
#include "Widgets/PlatformBar/PlatformBar.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QAbstractProxyModel>
#include <QAction>
#include <QComboBox>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QScopeGuard>
#include <QScreen>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QVariantMap>

#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <utility>

using namespace VNX;

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
} // namespace

TEST(MonitorPanel, GameObjectIdentityKeepsHistoricalNamesWithoutDuplicateRegistration)
{
    ensureApp();
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    VnMonitorHostSessionV1 host{};
    host.structSize = sizeof(host);
    ASSERT_EQ(VN_OK, Vn_MonitorBeginHostSession(&host));
    Monitor::EventCaptureController capture(nullptr);
    capture.setLocalRuntimeAvailable(true);
    MonitorPanel panel;
    panel.setCaptureController(&capture);
    capture.start();
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state());
    const auto waitFor = [&](const auto& ready) {
        QElapsedTimer timeout;
        timeout.start();
        while (!ready() && timeout.elapsed() < 2000) {
            Vn_Update();
            capture.pollLocal();
            QApplication::processEvents();
            QThread::msleep(1);
        }
        return ready();
    };
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_TRUE(waitFor([&] { return panel.visibleRowCount() == 1; }));
    EXPECT_TRUE(panel.table()->model()->index(0, MonitorCatalogModel::GameObject).data().toString().isEmpty());
    VnMonitorHostRegistrationV2 registration{};
    registration.structSize = sizeof(registration);
    registration.ownershipKind = VN_MONITOR_HOST_UNITY_EMITTER;
    registration.engineInstance = host.engineInstance;
    registration.sdkSession = host.sdkSession;
    registration.emitter = emitter;
    registration.localSequence = 1;
    registration.registrationRevision = 1;
    const QByteArray original("Player");
    registration.nameBytes = original.size();
    std::copy(original.begin(), original.end(), registration.name);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_TRUE(waitFor([&] {
        return panel.visibleRowCount() == 1 && panel.table()->model()->index(0, MonitorCatalogModel::GameObject).data().toString()
            == QStringLiteral("Player");
    }));
    QPersistentModelIndex registered(panel.table()->model()->index(0, MonitorCatalogModel::GameObject));
    registration.registrationRevision = 2;
    const QByteArray renamed("Renamed");
    std::fill(std::begin(registration.name), std::end(registration.name), 0);
    registration.nameBytes = renamed.size();
    std::copy(renamed.begin(), renamed.end(), registration.name);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    ASSERT_TRUE(waitFor([&] {
        return panel.visibleRowCount() == 2 && panel.table()->model()->index(1, MonitorCatalogModel::GameObject).data().toString()
            == QStringLiteral("Renamed");
    }));
    ASSERT_TRUE(registered.isValid());
    EXPECT_EQ(registered.data().toString(), QStringLiteral("Player"));
    for (int row = 0; row < 2; ++row) {
        EXPECT_EQ(panel.table()->model()->index(row, MonitorCatalogModel::Type).data().toString(), QStringLiteral("Game Object"));
        EXPECT_EQ(panel.table()->model()->index(row, MonitorCatalogModel::GameObjectId).data().toString(), QString::number(emitter));
        EXPECT_TRUE(panel.table()->model()->index(row, MonitorCatalogModel::ObjectId).data().toString().isEmpty());
    }
    EXPECT_EQ(panel.table()->model()->index(0, MonitorCatalogModel::Description).data().toString(), QStringLiteral("Registered"));
    EXPECT_EQ(panel.table()->model()->index(1, MonitorCatalogModel::Description).data().toString(), QStringLiteral("Unregistered"));

    VnEmitterHandle batchedEmitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&batchedEmitter));
    registration.emitter = batchedEmitter;
    registration.localSequence = 2;
    registration.registrationRevision = 1;
    registration.nameBytes = original.size();
    std::fill(std::begin(registration.name), std::end(registration.name), 0);
    std::copy(original.begin(), original.end(), registration.name);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    registration.registrationRevision = 2;
    registration.nameBytes = renamed.size();
    std::fill(std::begin(registration.name), std::end(registration.name), 0);
    std::copy(renamed.begin(), renamed.end(), registration.name);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_EQ(VN_OK, Vn_MonitorEndHostSession(host.engineInstance, host.sdkSession));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(batchedEmitter));
    ASSERT_TRUE(waitFor([&] {
        return panel.visibleRowCount() == 4 &&
            panel.table()->model()->index(2, MonitorCatalogModel::GameObject).data().toString() == QStringLiteral("Player") &&
            panel.table()->model()->index(3, MonitorCatalogModel::GameObject).data().toString() == QStringLiteral("Renamed");
    }));
    EXPECT_EQ(registered.data().toString(), QStringLiteral("Player"));
    for (int row = 2; row < 4; ++row)
        EXPECT_EQ(panel.table()->model()->index(row, MonitorCatalogModel::GameObjectId).data().toString(), QString::number(batchedEmitter));
}

TEST(MonitorPanel, GameObjectBaselineDoesNotInventRegistration)
{
    ensureApp();
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    VnMonitorHostSessionV1 host{};
    host.structSize = sizeof(host);
    ASSERT_EQ(VN_OK, Vn_MonitorBeginHostSession(&host));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnMonitorHostRegistrationV2 registration{};
    registration.structSize = sizeof(registration);
    registration.ownershipKind = VN_MONITOR_HOST_UNITY_EMITTER;
    registration.engineInstance = host.engineInstance;
    registration.sdkSession = host.sdkSession;
    registration.emitter = emitter;
    registration.localSequence = 1;
    registration.registrationRevision = 1;
    const QByteArray name("Existing");
    registration.nameBytes = name.size();
    std::copy(name.begin(), name.end(), registration.name);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    Monitor::EventCaptureController capture(nullptr);
    capture.setLocalRuntimeAvailable(true);
    MonitorPanel panel;
    panel.setCaptureController(&capture);
    capture.start();
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state());
    ASSERT_EQ(VN_OK, Vn_MonitorEndHostSession(host.engineInstance, host.sdkSession));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    QElapsedTimer timeout;
    timeout.start();
    while (panel.visibleRowCount() == 0 && timeout.elapsed() < 2000) {
        Vn_Update();
        capture.pollLocal();
        QApplication::processEvents();
        QThread::msleep(1);
    }
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state()) << capture.statusText().toStdString();
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_EQ(panel.table()->model()->index(0, MonitorCatalogModel::Description).data().toString(), QStringLiteral("Unregistered"));
    EXPECT_EQ(panel.table()->model()->index(0, MonitorCatalogModel::GameObject).data().toString(), QStringLiteral("Existing"));
    EXPECT_EQ(panel.table()->model()->index(0, MonitorCatalogModel::GameObjectId).data().toString(), QString::number(emitter));
}

TEST(MonitorPanel, LocalCaptureKeepsHistoryAcrossPanelLifetime)
{
    ensureApp();
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    Monitor::EventCaptureController capture(nullptr);
    capture.setLocalRuntimeAvailable(true);
    auto panel = std::make_unique<MonitorPanel>();
    panel->setCaptureController(&capture);
    capture.start();
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state());
    const auto waitFor = [&](const auto& ready) {
        QElapsedTimer timeout;
        timeout.start();
        while (!ready() && timeout.elapsed() < 2000) {
            Vn_Update();
            capture.pollLocal();
            QApplication::processEvents();
            QThread::msleep(1);
        }
        return ready();
    };
    const auto registrations = [&] {
        int count = 0;
        for (int row = 0; row < capture.rowCount(); ++row) {
            const auto& event = capture.eventAt(row);
            if (event.isExecution() && event.execution.factKind == VN_MONITOR_FACT_EMITTER_CREATED) ++count;
        }
        return count;
    };
    const auto createEmitter = [] {
        VnEmitterHandle emitter = 0;
        return Vn_CreateEmitter(&emitter);
    };
    ASSERT_EQ(VN_OK, createEmitter());
    ASSERT_TRUE(waitFor([&] { return registrations() == 1; }));
    ASSERT_GT(panel->visibleRowCount(), 0);
    panel->table()->selectRow(0);
    QPersistentModelIndex selected(panel->table()->currentIndex());
    int resets = 0;
    QObject::connect(panel->table()->model(), &QAbstractItemModel::modelReset, panel.get(), [&] { ++resets; });
    ASSERT_EQ(VN_OK, createEmitter());
    ASSERT_TRUE(waitFor([&] { return registrations() == 2; }));
    EXPECT_TRUE(selected.isValid());
    EXPECT_EQ(selected, panel->table()->currentIndex());
    EXPECT_EQ(resets, 0);
    panel.reset();
    ASSERT_EQ(VN_OK, createEmitter());
    ASSERT_TRUE(waitFor([&] { return registrations() == 3; }));
    panel = std::make_unique<MonitorPanel>();
    panel->setCaptureController(&capture);
    const auto visible = panel->visibleRowCount();
    EXPECT_GT(visible, 0);
    capture.stop();
    ASSERT_TRUE(waitFor([&] { return capture.state() == Monitor::EventCaptureState::Stopped; }));
    const auto retained = capture.rowCount();
    EXPECT_EQ(panel->visibleRowCount(), visible);
    ASSERT_EQ(VN_OK, createEmitter());
    ASSERT_EQ(VN_OK, Vn_Update());
    capture.pollLocal();
    EXPECT_EQ(capture.rowCount(), retained);
    capture.start();
    EXPECT_EQ(panel->visibleRowCount(), 0);
    ASSERT_EQ(VN_OK, createEmitter());
    ASSERT_TRUE(waitFor([&] { return registrations() == 1; }));
    panel->findChild<QToolButton*>(QStringLiteral("MonitorClear"))->click();
    EXPECT_EQ(capture.state(), Monitor::EventCaptureState::Capturing);
    EXPECT_EQ(capture.rowCount(), 0);
    ASSERT_EQ(VN_OK, createEmitter());
    ASSERT_TRUE(waitFor([&] { return registrations() == 1; }));
}

TEST(MonitorPanel, ConcurrentPreviewKeepsSelectedContentAndPlaybackIdentity)
{
    ensureApp();
    auto& engine = AudioEngine::instance();
    engine.shutdown();
    auto& projects = ProjectManager::instance();
    if (projects.isProjectOpen()) projects.closeProject();
    AssetRegistry::instance().clear();
    QTemporaryDir directory(QStringLiteral(HARKEN_TEMP_DIR "/monitor-preview-XXXXXX"));
    ASSERT_TRUE(directory.isValid());
    const auto cleanup = qScopeGuard([&] {
        engine.shutdown();
        projects.closeProject();
        QApplication::processEvents();
        AssetRegistry::instance().clear();
    });
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_TRUE(engine.startOutput());
    ProjectConfig projectConfig;
    projectConfig.name = QStringLiteral("MonitorPreview");
    projectConfig.location = directory.path();
    QString projectError;
    QObject errorContext;
    QObject::connect(&projects, &ProjectManager::errorOccurred, &errorContext,
        [&](const QString& error) { projectError = error; });
    ASSERT_TRUE(projects.createProject(projectConfig)) << projectError.toStdString();
    auto* layer = projects.createItem(nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* container = projects.createItem(layer, QStringLiteral("Footsteps"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    auto* selection = container->comp<VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Random;
    selection->behavior.discipline = RandomDiscipline::Draw;
    std::array<ProjectItem*, 2> sounds{};
    const QDir sources(QDir(QFileInfo(projects.currentProject()->projectPath).absolutePath())
        .filePath(QStringLiteral("AudioSources")));
    for (unsigned i = 0; i != sounds.size(); ++i) {
        const QString name = QStringLiteral("Footstep_%1").arg(i + 1);
        const QString path = sources.filePath(name + QStringLiteral(".wav"));
        QFile wav(path);
        ASSERT_TRUE(wav.open(QIODevice::WriteOnly));
        QDataStream stream(&wav);
        stream.setByteOrder(QDataStream::LittleEndian);
        constexpr quint32 frames = 4096;
        stream.writeRawData("RIFF", 4);
        stream << quint32(36 + frames * 2);
        stream.writeRawData("WAVEfmt ", 8);
        stream << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
               << quint32(96000) << quint16(2) << quint16(16);
        stream.writeRawData("data", 4);
        stream << quint32(frames * 2);
        for (quint32 frame = 0; frame != frames; ++frame) stream << qint16(4096 * (i + 1));
        ASSERT_EQ(QDataStream::Ok, stream.status());
        wav.close();
        AssetInfo asset;
        asset.sourceID = 93001 + i;
        asset.relativePath = QFileInfo(path).fileName();
        asset.absolutePath = path;
        ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
        sounds[i] = projects.createAudioItem(container, name, path, asset.sourceID);
        ASSERT_NE(sounds[i], nullptr);
    }
    auto* track = projects.createItem(layer, QStringLiteral("Delayed"), ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* delayedSound = projects.createAudioItem(track, QStringLiteral("Delayed Footstep"),
        sources.filePath(QStringLiteral("Footstep_1.wav")), 93001);
    ASSERT_NE(delayedSound, nullptr);
    auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    ASSERT_EQ(timeline->clips.items.size(), 1u);
    timeline->clips.items.front().start = 32000000;
    timeline->clips.items.front().duration = 85333333;
    ProjectManager::syncProjectToRuntime(projects.currentProject());
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    Monitor::EventCaptureController capture(nullptr);
    capture.setLocalRuntimeAvailable(true);
    capture.setProjectObjects(ProjectObjectIndex::fromProject(projects.currentProject()));
    MonitorPanel panel;
    panel.setCaptureController(&capture);
    float peak = 0;
    quint64 renderedFrames = 0;
    const auto waitFor = [&](const auto& ready) -> ::testing::AssertionResult {
        QElapsedTimer timeout;
        timeout.start();
        const auto firstFrame = renderedFrames;
        while (!ready() && timeout.elapsed() < 3000) {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            const auto beforeRender = Vn_Update();
            if (beforeRender != VN_OK) return ::testing::AssertionFailure() << "Vn_Update before render: " << beforeRender;
            runtime->nullBackend.Pump(1);
            renderedFrames += runtime->nullBackend.BlockFrames();
            const auto* block = runtime->nullBackend.LastBlock();
            const auto samples = runtime->nullBackend.BlockFrames() * runtime->nullBackend.Channels();
            for (uint32_t sample = 0; sample != samples; ++sample) peak = std::max(peak, std::abs(block[sample]));
            const auto afterRender = Vn_Update();
            if (afterRender != VN_OK) return ::testing::AssertionFailure() << "Vn_Update after render: " << afterRender;
            capture.pollLocal();
            QApplication::processEvents();
            QThread::msleep(1);
        }
        if (ready()) return ::testing::AssertionSuccess();
        auto failure = ::testing::AssertionFailure();
        failure << "Capture: " << capture.statusText().toStdString()
                << "; rendered frames: " << renderedFrames - firstFrame
                << "; visible records: " << panel.visibleRowCount();
        for (int row = 0; row < capture.rowCount(); ++row) {
            const auto& event = capture.eventAt(row);
            if (!event.isExecution()) continue;
            const auto& fact = event.execution;
            if (fact.factKind != VN_MONITOR_FACT_PLAYABLE_RETIRED &&
                fact.factKind != VN_MONITOR_FACT_PLAYABLE_CANCELED &&
                fact.factKind != VN_MONITOR_FACT_VOICE_ENDED) continue;
            failure << "\nFact " << fact.factKind << "; subject: " << fact.subjectNamespace
                    << "; instance: " << event.header.instanceHandle << "; root: " << fact.root
                    << "; voice: " << fact.logicalVoice << "; terminal: "
                    << (Monitor::monitorFactHas(fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND)
                        ? std::to_string(fact.terminalKind) : "not reported")
                    << "; result: " << fact.result << "; description: " << event.playbackDescription.toStdString();
        }
        return failure;
    };
    auto& sync = engine.runtimeSyncCoordinator();
    ASSERT_TRUE(waitFor([&] {
        return sync.barrier(sync.currentAuthoringRevision()).state == RuntimeSyncCoordinator::RevisionBarrierState::Ready;
    }));
    capture.start();
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state());
    std::array<quint64, 2> handles{};
    for (auto& handle : handles) {
        const auto started = engine.playPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableContainer, container->runtimeId());
        ASSERT_EQ(HarkenEditor::Bridge::kPreviewOk, started.status);
        handle = started.handle;
        ASSERT_NE(handle, 0u);
    }
    ASSERT_NE(handles[0], handles[1]);
    const auto voiceRows = [&] {
        std::vector<int> rows;
        for (int row = 0; row < capture.rowCount(); ++row) {
            const auto& event = capture.eventAt(row);
            if (event.isExecution() && event.execution.factKind == VN_MONITOR_FACT_VOICE_LIVE) rows.push_back(row);
        }
        return rows;
    };
    const auto playbackRows = [&](const QString& description, uint32_t contentKind = 0) {
        std::vector<int> rows;
        for (int row = 0; row < capture.rowCount(); ++row) {
            const auto& event = capture.eventAt(row);
            if (event.playbackDescription == description &&
                (contentKind == 0 || event.execution.contentKind == contentKind)) rows.push_back(row);
        }
        return rows;
    };
    ASSERT_TRUE(waitFor([&] {
        return voiceRows().size() >= handles.size() && playbackRows(QStringLiteral("Playing")).size() == handles.size() * 2
            && peak > 0.01f;
    }));
    std::set<quint64> roots, voices;
    std::vector<Monitor::CapturedEvent> liveVoices;
    for (const int row : voiceRows()) {
        const auto& event = capture.eventAt(row);
        liveVoices.push_back(event);
        const auto& fact = event.execution;
        ASSERT_TRUE(Monitor::monitorFactHas(fact, VN_MONITOR_FACT_FIELD_ROOT));
        ASSERT_TRUE(Monitor::monitorFactHas(fact, VN_MONITOR_FACT_FIELD_LOGICAL_VOICE));
        ASSERT_TRUE(Monitor::monitorFactHas(fact, VN_MONITOR_FACT_FIELD_SERVICE_LEAF));
        roots.insert(fact.root);
        voices.insert(fact.logicalVoice);
        const auto selected = std::find_if(sounds.begin(), sounds.end(), [&](const auto* sound) {
            return sound->runtimeId() == event.header.stableId;
        });
        ASSERT_NE(sounds.end(), selected);
        EXPECT_EQ((*selected)->name, event.objectName);
        EXPECT_EQ(QUuid(projects.currentProject()->projectId), event.projectGuid);
        EXPECT_EQ((*selected)->objectID, capture.matchingProjectObject(event));
        bool parent = false, leaf = false;
        for (int candidate = 0; candidate < capture.rowCount(); ++candidate) {
            const auto& related = capture.eventAt(candidate);
            if (!related.isExecution() || related.playbackDescription != QStringLiteral("Playing")) continue;
            parent |= related.header.instanceHandle == fact.root && related.header.stableId == container->runtimeId();
            leaf |= related.header.instanceHandle == fact.serviceLeaf
                && related.header.playableInstanceHandle == fact.root
                && related.header.stableId == event.header.stableId;
            if (related.header.instanceHandle == fact.root || related.header.instanceHandle == fact.serviceLeaf)
                EXPECT_EQ(related.playbackRequestId(), event.playbackRequestId());
        }
        EXPECT_TRUE(parent);
        EXPECT_TRUE(leaf);
    }
    EXPECT_EQ(handles.size(), roots.size());
    EXPECT_EQ(handles.size(), voices.size());
    EXPECT_FALSE(roots.contains(0));
    EXPECT_FALSE(voices.contains(0));
    auto* proxy = qobject_cast<QAbstractProxyModel*>(panel.table()->model());
    ASSERT_NE(proxy, nullptr);
    for (const auto row : voiceRows())
        EXPECT_FALSE(proxy->mapFromSource(proxy->sourceModel()->index(row, 0)).isValid());
    const auto soundRows = playbackRows(QStringLiteral("Playing"), VN_MONITOR_CONTENT_SOUND);
    ASSERT_EQ(soundRows.size(), handles.size());
    const int soundRow = soundRows.front();
    const auto visible = proxy->mapFromSource(proxy->sourceModel()->index(soundRow, 0));
    ASSERT_TRUE(visible.isValid());
    panel.table()->selectRow(visible.row());
    harkenstudio::ObjectId located{};
    panel.setJumpLocator([&](harkenstudio::ObjectId id) { located = id; });
    ASSERT_TRUE(panel.jumpCurrentRow());
    EXPECT_EQ(capture.eventAt(soundRow).header.stableId, located);
    const auto originalSound = capture.eventAt(soundRow);
    ASSERT_TRUE(waitFor([&] {
        return playbackRows(QStringLiteral("Finished")).size() == handles.size() * 2 &&
            std::all_of(liveVoices.begin(), liveVoices.end(), [&](const auto& live) {
                for (int row = 0; row < capture.rowCount(); ++row) {
                    const auto& event = capture.eventAt(row);
                    if (event.isExecution() && event.execution.factKind == VN_MONITOR_FACT_VOICE_ENDED
                        && event.header.engineMonitorInstanceId == live.header.engineMonitorInstanceId
                        && event.execution.logicalVoice == live.execution.logicalVoice) return true;
                }
                return false;
            });
    }));
    for (int row = 0; row < capture.rowCount(); ++row) {
        const auto& event = capture.eventAt(row);
        if (!event.isExecution() || event.execution.factKind != VN_MONITOR_FACT_VOICE_ENDED) continue;
        const auto live = std::find_if(liveVoices.begin(), liveVoices.end(), [&](const auto& started) {
            return started.header.engineMonitorInstanceId == event.header.engineMonitorInstanceId
                && started.execution.logicalVoice == event.execution.logicalVoice;
        });
        if (live == liveVoices.end()) continue;
        EXPECT_EQ(live->execution.root, event.execution.root);
        EXPECT_EQ(live->objectId(), event.objectId());
        EXPECT_EQ(live->projectGuid, event.projectGuid);
        EXPECT_EQ(live->objectName, event.objectName);
        EXPECT_EQ(capture.matchingProjectObject(*live), capture.matchingProjectObject(event));
    }
    std::set<quint64> playbackRequestIds;
    for (const auto row : playbackRows(QStringLiteral("Playing"))) {
        const auto& started = capture.eventAt(row);
        EXPECT_NE(started.playbackRequestId(), 0u);
        playbackRequestIds.insert(started.playbackRequestId());
        EXPECT_EQ(started.gameObjectId(), VN_MONITOR_STUDIO_DEFAULT_GAME_OBJECT_ID);
        EXPECT_EQ(started.gameObjectName, QStringLiteral("Emitter"));
        int matches = 0;
        for (const auto finishedRow : playbackRows(QStringLiteral("Finished"))) {
            const auto& finished = capture.eventAt(finishedRow);
            if (finished.header.engineMonitorInstanceId != started.header.engineMonitorInstanceId ||
                finished.header.instanceHandle != started.header.instanceHandle) continue;
            ++matches;
            EXPECT_EQ(started.playbackRequestId(), finished.playbackRequestId());
            EXPECT_EQ(started.objectId(), finished.objectId());
            EXPECT_EQ(started.objectName, finished.objectName);
            EXPECT_EQ(started.gameObjectId(), finished.gameObjectId());
            EXPECT_EQ(started.gameObjectName, finished.gameObjectName);
            EXPECT_EQ(started.execution.root, finished.execution.root);
            EXPECT_LE(started.timestampNs(), finished.timestampNs());
        }
        EXPECT_EQ(matches, 1);
    }
    EXPECT_EQ(playbackRequestIds.size(), handles.size());
    int registeredEmitters = 0;
    for (int row = 0; row < capture.rowCount(); ++row) {
        const auto& event = capture.eventAt(row);
        if (event.type() != Monitor::DesignerEventType::GameObject || !event.isDesignerEvent()) continue;
        EXPECT_EQ(event.execution.factKind, VN_MONITOR_FACT_EMITTER_CREATED);
        EXPECT_EQ(event.gameObjectName, QStringLiteral("Emitter"));
        EXPECT_EQ(event.gameObjectId(), VN_MONITOR_STUDIO_DEFAULT_GAME_OBJECT_ID);
        ++registeredEmitters;
    }
    EXPECT_EQ(registeredEmitters, 1);
    EXPECT_EQ(panel.visibleRowCount(), static_cast<int>(handles.size() * 4) + registeredEmitters);
    capture.stop();
    ASSERT_TRUE(waitFor([&] { return capture.state() == Monitor::EventCaptureState::Stopped; }));
    const Monitor::CapturedEvent* retainedSound = nullptr;
    for (int row = 0; row < capture.rowCount(); ++row) {
        const auto& event = capture.eventAt(row);
        if (event.header.engineMonitorInstanceId == originalSound.header.engineMonitorInstanceId
            && event.header.producerId == originalSound.header.producerId
            && event.header.sourceSequence == originalSound.header.sourceSequence) {
            retainedSound = &event;
            break;
        }
    }
    ASSERT_NE(retainedSound, nullptr);
    EXPECT_EQ(originalSound.objectName, retainedSound->objectName);
    EXPECT_EQ(originalSound.objectId(), retainedSound->objectId());
    EXPECT_EQ(originalSound.execution.root, retainedSound->execution.root);
    EXPECT_EQ(originalSound.execution.logicalVoice, retainedSound->execution.logicalVoice);
    EXPECT_FALSE(capture.incomplete());
    const QString containerImagePath = qEnvironmentVariable("HARKEN_MONITOR_CONTAINER_CAPTURE_IMAGE");
    if (!containerImagePath.isEmpty()) {
        panel.table()->clearSelection();
        panel.table()->setCurrentIndex({});
        panel.table()->scrollToTop();
        panel.resize(1400, 420);
        panel.show();
        QApplication::processEvents();
        EXPECT_TRUE(panel.grab().save(containerImagePath));
    }
    capture.start();
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state());
    const auto single = engine.playPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableSound, sounds.front()->runtimeId());
    ASSERT_EQ(HarkenEditor::Bridge::kPreviewOk, single.status);
    ASSERT_TRUE(waitFor([&] { return playbackRows(QStringLiteral("Finished")).size() == 1; }));
    capture.stop();
    ASSERT_TRUE(waitFor([&] { return capture.state() == Monitor::EventCaptureState::Stopped; }));
    ASSERT_EQ(panel.visibleRowCount(), 2);
    ASSERT_EQ(playbackRows(QStringLiteral("Playing")).size(), 1u);
    const auto& playing = capture.eventAt(playbackRows(QStringLiteral("Playing")).front());
    const auto& finished = capture.eventAt(playbackRows(QStringLiteral("Finished")).front());
    const auto singleRoot = single.handle & ~(quint64{1} << 63);
    EXPECT_EQ(playing.header.instanceHandle, singleRoot);
    EXPECT_EQ(finished.header.instanceHandle, singleRoot);
    EXPECT_EQ(playing.execution.root, singleRoot);
    EXPECT_EQ(finished.execution.root, singleRoot);
    EXPECT_EQ(playing.objectId(), sounds.front()->runtimeId());
    EXPECT_EQ(playing.objectId(), finished.objectId());
    ASSERT_NE(playing.playbackRequestId(), 0u);
    EXPECT_FALSE(playbackRequestIds.contains(playing.playbackRequestId()));
    EXPECT_EQ(playing.playbackRequestId(), finished.playbackRequestId());
    EXPECT_EQ(playing.gameObjectId(), VN_MONITOR_STUDIO_DEFAULT_GAME_OBJECT_ID);
    EXPECT_EQ(playing.gameObjectId(), originalSound.gameObjectId());
    EXPECT_EQ(playing.gameObjectId(), finished.gameObjectId());
    EXPECT_EQ(playing.gameObjectName, QStringLiteral("Emitter"));
    EXPECT_EQ(playing.gameObjectName, finished.gameObjectName);
    const auto* tableModel = panel.table()->model();
    EXPECT_EQ(tableModel->columnCount(), 9);
    EXPECT_EQ(tableModel->index(0, MonitorCatalogModel::Description).data().toString(), QStringLiteral("Playing"));
    EXPECT_EQ(tableModel->index(1, MonitorCatalogModel::Description).data().toString(), QStringLiteral("Finished"));
    for (int row = 0; row < 2; ++row) {
        EXPECT_EQ(tableModel->index(row, MonitorCatalogModel::RequestId).data().toString(), QString::number(playing.playbackRequestId()));
        EXPECT_EQ(tableModel->index(row, MonitorCatalogModel::Object).data().toString(), sounds.front()->name);
        EXPECT_EQ(tableModel->index(row, MonitorCatalogModel::ObjectId).data().toString(), objectIdString(playing.objectId()));
        EXPECT_EQ(tableModel->index(row, MonitorCatalogModel::GameObject).data().toString(), QStringLiteral("Emitter"));
        EXPECT_EQ(tableModel->index(row, MonitorCatalogModel::GameObjectId).data().toString(), QString::number(playing.gameObjectId()));
    }
    EXPECT_FALSE(capture.incomplete());
    const QString imagePath = qEnvironmentVariable("HARKEN_MONITOR_CAPTURE_IMAGE");
    if (!imagePath.isEmpty()) {
        panel.resize(1400, 360);
        panel.show();
        QApplication::processEvents();
        EXPECT_TRUE(panel.grab().save(imagePath));
    }
    capture.start();
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state());
    const auto interrupted = engine.playPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableSound, sounds.front()->runtimeId());
    ASSERT_EQ(HarkenEditor::Bridge::kPreviewOk, interrupted.status);
    ASSERT_TRUE(waitFor([&] { return playbackRows(QStringLiteral("Playing")).size() == 1; }));
    const auto playingToStop = capture.eventAt(playbackRows(QStringLiteral("Playing")).front());
    ASSERT_NE(playingToStop.playbackRequestId(), 0u);
    ASSERT_EQ(HarkenEditor::Bridge::kPreviewOk, engine.stopPreviewPlayable(interrupted.handle));
    ASSERT_TRUE(waitFor([&] { return playbackRows(QStringLiteral("Stopped")).size() == 1; }));
    capture.stop();
    ASSERT_TRUE(waitFor([&] { return capture.state() == Monitor::EventCaptureState::Stopped; }));
    ASSERT_EQ(panel.visibleRowCount(), 2);
    EXPECT_TRUE(playbackRows(QStringLiteral("Finished")).empty());
    const auto& stopped = capture.eventAt(playbackRows(QStringLiteral("Stopped")).front());
    const auto interruptedRoot = interrupted.handle & ~(quint64{1} << 63);
    EXPECT_EQ(playingToStop.header.instanceHandle, interruptedRoot);
    EXPECT_EQ(stopped.header.instanceHandle, interruptedRoot);
    EXPECT_EQ(playingToStop.playbackRequestId(), stopped.playbackRequestId());
    EXPECT_EQ(playingToStop.objectId(), stopped.objectId());
    EXPECT_EQ(playingToStop.objectName, stopped.objectName);
    EXPECT_EQ(playingToStop.gameObjectId(), stopped.gameObjectId());
    EXPECT_EQ(playingToStop.gameObjectName, stopped.gameObjectName);
    EXPECT_EQ(stopped.gameObjectId(), VN_MONITOR_STUDIO_DEFAULT_GAME_OBJECT_ID);
    EXPECT_EQ(tableModel->index(0, MonitorCatalogModel::Description).data().toString(), QStringLiteral("Playing"));
    EXPECT_EQ(tableModel->index(1, MonitorCatalogModel::Description).data().toString(), QStringLiteral("Stopped"));
    for (int row = 0; row < 2; ++row)
        EXPECT_EQ(tableModel->index(row, MonitorCatalogModel::RequestId).data().toString(), QString::number(playingToStop.playbackRequestId()));
    EXPECT_FALSE(capture.incomplete());

    capture.start();
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state());
    peak = 0;
    const auto beforeDelayed = renderedFrames;
    const auto delayed = engine.playPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableTrack, track->runtimeId());
    ASSERT_EQ(HarkenEditor::Bridge::kPreviewOk, delayed.status);
    ASSERT_TRUE(waitFor([&] { return renderedFrames > beforeDelayed; }));
    EXPECT_EQ(playbackRows(QStringLiteral("Playing"), VN_MONITOR_CONTENT_TRACK).size(), 1u);
    EXPECT_TRUE(playbackRows(QStringLiteral("Playing"), VN_MONITOR_CONTENT_SOUND).empty());
    EXPECT_EQ(peak, 0.0f);
    ASSERT_TRUE(waitFor([&] { return playbackRows(QStringLiteral("Finished")).size() == 2; }));
    ASSERT_EQ(playbackRows(QStringLiteral("Playing"), VN_MONITOR_CONTENT_SOUND).size(), 1u);
    const auto& delayedPlaying = capture.eventAt(playbackRows(QStringLiteral("Playing"), VN_MONITOR_CONTENT_SOUND).front());
    const auto& trackPlaying = capture.eventAt(playbackRows(QStringLiteral("Playing"), VN_MONITOR_CONTENT_TRACK).front());
    EXPECT_EQ(delayedPlaying.objectId(), delayedSound->runtimeId());
    EXPECT_EQ(delayedPlaying.playbackRequestId(), trackPlaying.playbackRequestId());
    EXPECT_GT(delayedPlaying.timestampNs(), trackPlaying.timestampNs());
    EXPECT_GT(peak, 0.01f);
    capture.stop();
    ASSERT_TRUE(waitFor([&] { return capture.state() == Monitor::EventCaptureState::Stopped; }));
    EXPECT_EQ(panel.visibleRowCount(), 4);

    capture.start();
    ASSERT_EQ(Monitor::EventCaptureState::Capturing, capture.state());
    const auto cancelled = engine.playPreviewPlayable(HarkenEditor::Bridge::kPreviewPlayableTrack, track->runtimeId());
    ASSERT_EQ(HarkenEditor::Bridge::kPreviewOk, cancelled.status);
    ASSERT_EQ(HarkenEditor::Bridge::kPreviewOk, engine.stopPreviewPlayable(cancelled.handle));
    ASSERT_TRUE(waitFor([&] { return !playbackRows(QStringLiteral("Stopped"), VN_MONITOR_CONTENT_TRACK).empty(); }));
    capture.stop();
    ASSERT_TRUE(waitFor([&] { return capture.state() == Monitor::EventCaptureState::Stopped; }));
    EXPECT_TRUE(playbackRows(QStringLiteral("Playing"), VN_MONITOR_CONTENT_SOUND).empty());
    EXPECT_TRUE(playbackRows(QStringLiteral("Finished"), VN_MONITOR_CONTENT_SOUND).empty());
    EXPECT_FALSE(capture.incomplete());
}

TEST(MonitorPanel, LiveParamsRestoreTypeLevelAndScrolling)
{
    ensureApp();
    Monitor::EventCaptureController capture(nullptr);
    MonitorPanel panel;
    panel.setCaptureController(&capture);
    QVariantMap params{
        {QStringLiteral("filterType"), static_cast<int>(Monitor::DesignerEventType::Variable)},
        {QStringLiteral("filterLevel"), VN_LOG_ERROR},
        {QStringLiteral("filterText"), QStringLiteral("overload")},
        {QStringLiteral("autoScroll"), false},
    };
    panel.restoreParams(params);
    const auto restored = panel.snapshotParams();
    for (auto it = params.cbegin(); it != params.cend(); ++it) EXPECT_EQ(restored.value(it.key()), it.value());
    auto* type = panel.findChild<QComboBox*>(QStringLiteral("MonitorTypeFilter"));
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->currentData().toInt(), static_cast<int>(Monitor::DesignerEventType::Variable));
    panel.restoreParams({{QStringLiteral("filterType"), QStringLiteral("All")}});
    EXPECT_EQ(type->currentIndex(), 0);
    panel.restoreParams({{QStringLiteral("filterType"), 99}});
    EXPECT_EQ(type->currentIndex(), 0);
}

TEST(MonitorPanel, ToolbarCapturesCurrentProjectAcrossPanelAndLayoutChanges)
{
    ensureApp();
    ActionManager::instance().clearAllForTesting();
    auto& projects = ProjectManager::instance();
    if (projects.isProjectOpen()) projects.closeProject();
    auto& engine = AudioEngine::instance();
    engine.shutdown();
    HarkenEditor::Bridge::EngineSessionConfig engineConfig;
    engineConfig.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(engineConfig));
    ASSERT_TRUE(engine.startOutput());
    QTemporaryDir directory(QStringLiteral(HARKEN_TEMP_DIR "/event-toolbar-XXXXXX"));
    ASSERT_TRUE(directory.isValid());
    auto window = std::make_unique<MainWindow>();
    const auto cleanup = qScopeGuard([&] {
        projects.closeProject();
        QObject::disconnect(&UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        window.reset();
        ActionManager::instance().setTransactionCallbacks({});
        ActionManager::instance().clearAllForTesting();
        UndoRedoManager::instance().clear();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    });
    const QRect available = window->screen()->availableGeometry();
    window->resize(available.size().boundedTo(QSize(1200, 800)) - QSize(40, 40));
    window->move(available.topLeft() + QPoint(20, 20));
    window->show();
    auto* toolbar = window->findChild<PlatformBar*>(QStringLiteral("platformBar"));
    auto* button = window->findChild<QToolButton*>(QStringLiteral("captureButton"));
    auto* capture = window->eventCaptureController();
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(button, nullptr);
    ASSERT_NE(capture, nullptr);
    QApplication::processEvents();
    EXPECT_TRUE(toolbar->isAncestorOf(button));
    EXPECT_TRUE(button->isEnabled());
    EXPECT_FALSE(button->isChecked());
    EXPECT_FALSE(button->icon().isNull());
    EXPECT_EQ(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
    auto* view = window->findChild<QMenu*>(QStringLiteral("viewsMenu"));
    ASSERT_NE(view, nullptr);
    view->popup(window->mapToGlobal(QPoint(20, 40)));
    QApplication::processEvents();
    auto* catalog = view->findChild<QAction*>(QStringLiteral("viewToggle_MonitorDock"));
    ASSERT_NE(catalog, nullptr);
    EXPECT_TRUE(catalog->isEnabled());
    const auto clickMenuAction = [view](QAction* action) {
        const QPoint position = view->actionGeometry(action).center();
        QMouseEvent press(QEvent::MouseButtonPress, position, view->mapToGlobal(position),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, position, view->mapToGlobal(position),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view, &press);
        QApplication::sendEvent(view, &release);
    };
    clickMenuAction(catalog);
    QApplication::processEvents();
    auto* idlePanel = window->findChild<MonitorPanel*>();
    ASSERT_NE(idlePanel, nullptr);
    EXPECT_TRUE(idlePanel->isVisible());
    EXPECT_TRUE(idlePanel->window()->isVisible());
    EXPECT_FALSE(idlePanel->visibleRegion().isEmpty());
    const QRect frame = idlePanel->window()->frameGeometry();
    EXPECT_TRUE(available.contains(frame)) << "Window frame: "
        << frame.x() << ", " << frame.y() << ", " << frame.width() << ", " << frame.height()
        << "; available screen: " << available.x() << ", " << available.y()
        << ", " << available.width() << ", " << available.height();
    EXPECT_GT(idlePanel->table()->viewport()->height(), 0);
    EXPECT_FALSE(capture->canStop());

    ProjectConfig config;
    config.name = QStringLiteral("EventToolbar");
    config.location = directory.path();
    ASSERT_TRUE(projects.createProject(config));
    auto* variable = projects.createItem(nullptr, QStringLiteral("Intensity"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    variable->comp<VVariable>()->writers = 2;
    auto* action = projects.createItem(nullptr, QStringLiteral("SetIntensity"), ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    VActionStep step;
    step.type = VActionStep::Type::SetVariable;
    step.targetObjectID = variable->objectID;
    step.targetName = variable->name;
    step.value = 0.25f;
    step.scope = VariableScope::Global;
    action->comp<VAction>()->steps = {step};
    ProjectManager::syncProjectToRuntime(projects.currentProject());
    const auto waitFor = [&](const auto& ready) {
        QElapsedTimer timer;
        timer.start();
        while (!ready() && timer.elapsed() < 3000) {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            harken::detail::ActiveEngine()->nullBackend.Pump(1);
            capture->pollLocal();
            QApplication::processEvents();
            QThread::msleep(1);
        }
        return ready();
    };
    auto& sync = engine.runtimeSyncCoordinator();
    ASSERT_TRUE(waitFor([&] {
        return sync.barrier(sync.currentAuthoringRevision()).state == RuntimeSyncCoordinator::RevisionBarrierState::Ready;
    }));
    auto* dock = window->findChild<ads::CDockWidget*>(QStringLiteral("MonitorDock"));
    ASSERT_NE(dock, nullptr);
    dock->toggleView(false);
    QApplication::processEvents();
    ASSERT_TRUE(dock->isClosed());
    ASSERT_FALSE(idlePanel->isVisible());
    button->click();
    QApplication::processEvents();
    ASSERT_EQ(capture->state(), Monitor::EventCaptureState::Capturing);
    EXPECT_TRUE(button->isChecked());
    auto* panel = window->findChild<MonitorPanel*>();
    ASSERT_NE(panel, nullptr);
    EXPECT_TRUE(dock->isClosed());
    EXPECT_FALSE(panel->isVisible());
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(action->runtimeId());
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    const auto request = harken::detail::ActiveEngine()->monitorRequests.LastSequence();
    ASSERT_TRUE(waitFor([&] { return capture->rowCount() > 0; }));
    const auto matchingRow = [&] {
        for (int i = 0; i < capture->rowCount(); ++i)
            if (capture->eventAt(i).isDesignerEvent() &&
                capture->eventAt(i).type() == Monitor::DesignerEventType::Variable &&
                capture->eventAt(i).objectId() == variable->runtimeId()) return i;
        return -1;
    };
    ASSERT_TRUE(waitFor([&] { return matchingRow() >= 0; }));
    const int row = matchingRow();
    EXPECT_EQ(capture->eventAt(row).objectName, QStringLiteral("Intensity"));
    EXPECT_EQ(capture->matchingProjectObject(capture->eventAt(row)), variable->objectID);
    EXPECT_TRUE(dock->isClosed());
    EXPECT_FALSE(panel->isVisible());
    ASSERT_TRUE(window->setWorkspaceWindowVisible(QStringLiteral("Monitor"), true));
    QApplication::processEvents();
    EXPECT_FALSE(dock->isClosed());
    EXPECT_TRUE(panel->isVisible());
    EXPECT_FALSE(panel->visibleRegion().isEmpty());
    EXPECT_GT(panel->visibleRowCount(), 0);
    auto* proxy = qobject_cast<QAbstractProxyModel*>(panel->table()->model());
    ASSERT_NE(proxy, nullptr);
    std::set<Monitor::DesignerEventType> observedTypes;
    for (int i = 0; i < capture->rowCount(); ++i) {
        const auto& event = capture->eventAt(i);
        if (!event.isDesignerEvent() || event.playbackRequestId() != request) continue;
        const auto index = proxy->mapFromSource(proxy->sourceModel()->index(i, 0));
        ASSERT_TRUE(index.isValid());
        observedTypes.insert(event.type());
        EXPECT_EQ(proxy->index(index.row(), MonitorCatalogModel::RequestId).data().toString(), QString::number(request));
        EXPECT_EQ(proxy->index(index.row(), MonitorCatalogModel::Object).data().toString(),
            event.type() == Monitor::DesignerEventType::Action ? action->name : variable->name);
        EXPECT_EQ(event.objectId(), event.type() == Monitor::DesignerEventType::Action
            ? action->runtimeId() : variable->runtimeId());
        if (event.type() == Monitor::DesignerEventType::Step)
            EXPECT_TRUE(proxy->index(index.row(), MonitorCatalogModel::Description).data(Qt::ToolTipRole)
                .toString().contains(action->name));
    }
    EXPECT_EQ((std::set<Monitor::DesignerEventType>{Monitor::DesignerEventType::Action,
        Monitor::DesignerEventType::Step, Monitor::DesignerEventType::Variable}), observedTypes);
    const QString actionImagePath = qEnvironmentVariable("HARKEN_MONITOR_ACTION_CAPTURE_IMAGE");
    if (!actionImagePath.isEmpty()) {
        MonitorPanel imagePanel;
        imagePanel.setCaptureController(capture);
        imagePanel.resize(1400, 360);
        imagePanel.show();
        QApplication::processEvents();
        EXPECT_TRUE(imagePanel.grab().save(actionImagePath));
    }
    const auto visible = proxy->mapFromSource(proxy->sourceModel()->index(row, 0));
    ASSERT_TRUE(visible.isValid());
    panel->table()->selectRow(visible.row());
    EXPECT_TRUE(panel->jumpCurrentRow());
    ASSERT_TRUE(projects.renameItem(variable, QStringLiteral("Drive")));
    EXPECT_EQ(capture->eventAt(row).objectName, QStringLiteral("Intensity"));

    const QRect savedFrame = panel->window()->frameGeometry();
    ASSERT_TRUE(window->saveNamedLayout(QStringLiteral("MonitorPanelTest")));
    dock->toggleView(false);
    EXPECT_EQ(capture->state(), Monitor::EventCaptureState::Capturing);
    const auto count = capture->rowCount();
    VnEmitterHandle additionalEmitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&additionalEmitter));
    ASSERT_TRUE(waitFor([&] { return capture->rowCount() > count; }));
    ASSERT_TRUE(window->loadNamedLayout(QStringLiteral("MonitorPanelTest")));
    EXPECT_FALSE(dock->isClosed());
    EXPECT_TRUE(panel->isVisible());
    EXPECT_EQ(panel->window()->frameGeometry(), savedFrame);
    EXPECT_GT(panel->visibleRowCount(), 0);
    EXPECT_EQ(capture->state(), Monitor::EventCaptureState::Capturing);

    button->click();
    ASSERT_TRUE(waitFor([&] { return capture->state() == Monitor::EventCaptureState::Stopped; }));
    EXPECT_FALSE(button->isChecked());
    const auto retained = capture->rowCount();
    EXPECT_GT(retained, 0);
    panel->window()->resize(panel->window()->size() - QSize(80, 80));
    panel->window()->move(available.topLeft() + QPoint(8, 8));
    QApplication::processEvents();
    const QRect userFrame = panel->window()->frameGeometry();
    ASSERT_TRUE(available.contains(userFrame));
    dock->toggleView(false);
    ASSERT_TRUE(window->setWorkspaceWindowVisible(QStringLiteral("Monitor"), true));
    QApplication::processEvents();
    EXPECT_EQ(panel->window()->frameGeometry(), userFrame);
    panel->window()->move(available.bottomRight() + QPoint(100, 100));
    dock->toggleView(false);
    view->popup(window->mapToGlobal(QPoint(20, 40)));
    QApplication::processEvents();
    catalog = view->findChild<QAction*>(QStringLiteral("viewToggle_MonitorDock"));
    ASSERT_NE(catalog, nullptr);
    EXPECT_FALSE(catalog->isChecked());
    clickMenuAction(catalog);
    QApplication::processEvents();
    EXPECT_TRUE(panel->isVisible());
    EXPECT_FALSE(panel->visibleRegion().isEmpty());
    EXPECT_TRUE(available.contains(panel->window()->frameGeometry()));
    EXPECT_EQ(capture->state(), Monitor::EventCaptureState::Stopped);
    EXPECT_EQ(capture->rowCount(), retained);
    dock->toggleView(false);
    QApplication::processEvents();
    ASSERT_TRUE(dock->isClosed());
    ASSERT_FALSE(panel->isVisible());
    button->click();
    QApplication::processEvents();
    EXPECT_EQ(capture->state(), Monitor::EventCaptureState::Capturing);
    EXPECT_EQ(capture->rowCount(), 0);
    EXPECT_TRUE(dock->isClosed());
    EXPECT_FALSE(panel->isVisible());
    projects.closeProject();
    ASSERT_TRUE(waitFor([&] { return capture->state() == Monitor::EventCaptureState::Stopped; }));
    EXPECT_FALSE(button->isChecked());
}

TEST(MonitorPanel, RestoredPanelIsVisibleBeforeCaptureAndCanReopen)
{
    ensureApp();
    for (const bool floating : {false, true}) {
        SCOPED_TRACE(floating);
        QByteArray savedState;
        for (const bool restoring : {false, true}) {
            SCOPED_TRACE(restoring);
            QMainWindow window;
            window.resize(1200, 800);
            auto* docks = new ads::CDockManager(&window);
            WorkspaceWindowManager workspace(defaultWorkspaceWindowCatalog(), QtAdsWindowAdapter(docks, {}));
            Monitor::EventCaptureController capture(nullptr);
            workspace.registerSingletonFactory(QStringLiteral("Monitor"), {
                [&]() -> QWidget* {
                    auto* panel = new MonitorPanel();
                    panel->setCaptureController(&capture);
                    return panel;
                }, {}, {}});
            auto* dock = workspace.ensureSingleton(QStringLiteral("Monitor"));
            ASSERT_NE(dock, nullptr);
            window.show();
            QApplication::processEvents();
            if (restoring) {
                ASSERT_TRUE(docks->restoreState(savedState));
                EXPECT_EQ(workspace.materializeVisibleSingletons(), 1);
            } else {
                ASSERT_TRUE(workspace.showSingleton(QStringLiteral("Monitor")));
                if (floating) docks->addDockWidgetFloating(dock);
                savedState = docks->saveState();
            }
            QApplication::processEvents();
            ASSERT_NE(dock->widget(), nullptr);
            EXPECT_FALSE(dock->isClosed());
            EXPECT_TRUE(dock->widget()->isVisible());
            EXPECT_FALSE(dock->widget()->visibleRegion().isEmpty());
            EXPECT_FALSE(capture.canStop());
            EXPECT_FALSE(capture.canStart());
            ASSERT_TRUE(workspace.showSingleton(QStringLiteral("Monitor")));
            QApplication::processEvents();
            EXPECT_TRUE(dock->widget()->isVisible());
            ASSERT_TRUE(workspace.hideSingleton(QStringLiteral("Monitor")));
            ASSERT_TRUE(workspace.showSingleton(QStringLiteral("Monitor")));
            QApplication::processEvents();
            EXPECT_TRUE(dock->widget()->isVisible());
        }
    }
}
