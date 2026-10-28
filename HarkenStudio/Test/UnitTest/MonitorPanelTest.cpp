#include "Engine.h"
#include <harken/Harken.h>
#include "Widgets/Monitor/MonitorPanel.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "AssetPipeline/AssetRegistry.h"
#include "Components/VSelection.h"
#include "Core/AudioEngine.h"
#include "Core/ProjectManager.h"
#include "Core/RuntimeSyncCoordinator.h"
#include "RuntimeBridge/PreviewBridge.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QScopeGuard>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <vector>

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
    ProjectManager::syncProjectToRuntime(projects.currentProject());
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(runtime, nullptr);
    Monitor::EventCaptureController capture(nullptr);
    capture.setLocalRuntimeAvailable(true);
    capture.setProjectObjects(ProjectObjectIndex::fromProject(projects.currentProject()));
    MonitorPanel panel;
    panel.setCaptureController(&capture);
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
    ASSERT_TRUE(waitFor([&] {
        return voiceRows().size() >= handles.size();
    }));
    std::set<quint64> roots, voices;
    std::vector<Monitor::CapturedEvent> liveVoices;
    for (const int row : voiceRows()) {
        const auto& event = capture.eventAt(row);
        liveVoices.push_back(event);
        const auto& fact = event.execution;
        ASSERT_TRUE(Monitor::monitorFactHas(fact, VN_MONITOR_FACT_FIELD_ROOT));
        ASSERT_TRUE(Monitor::monitorFactHas(fact, VN_MONITOR_FACT_FIELD_LOGICAL_VOICE));
        roots.insert(fact.root);
        voices.insert(fact.logicalVoice);
        const auto selected = std::find_if(sounds.begin(), sounds.end(), [&](const auto* sound) {
            return sound->runtimeId() == event.header.stableId;
        });
        ASSERT_NE(sounds.end(), selected);
        EXPECT_EQ(QUuid(projects.currentProject()->projectId), event.projectGuid);
        EXPECT_EQ((*selected)->objectID, capture.matchingProjectObject(event));
    }
    EXPECT_EQ(handles.size(), roots.size());
    EXPECT_EQ(liveVoices.size(), voices.size());
    EXPECT_FALSE(roots.contains(0));
    EXPECT_FALSE(voices.contains(0));
    ASSERT_TRUE(waitFor([&] {
        return std::all_of(liveVoices.begin(), liveVoices.end(), [&](const auto& live) {
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
        EXPECT_EQ(capture.matchingProjectObject(*live), capture.matchingProjectObject(event));
    }

    capture.stop();
    ASSERT_FALSE(liveVoices.empty());
    const auto selectedSound = std::find_if(sounds.begin(), sounds.end(), [&](const auto* sound) {
        return sound->runtimeId() == liveVoices.front().objectId();
    });
    ASSERT_NE(sounds.end(), selectedSound);
    const auto objectId = (*selectedSound)->objectID;
    const QString capturedName = (*selectedSound)->name;
    ObjectRefNavigation navigation(&panel);
    std::vector<uint32_t> located, opened;
    QObject::connect(&navigation, &ObjectRefNavigation::locateRequested, &panel,
        [&](RefDomain domain, uint32_t id) {
            EXPECT_EQ(domain, RefDomain::ProjectObject);
            located.push_back(id);
        });
    QObject::connect(&navigation, &ObjectRefNavigation::navigateRequested, &panel,
        [&](RefDomain domain, uint32_t id) {
            EXPECT_EQ(domain, RefDomain::ProjectObject);
            opened.push_back(id);
        });
    panel.resize(1400, 600);
    panel.show();
    QApplication::processEvents();
    auto* table = panel.table();
    QPersistentModelIndex objectIndex;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const auto index = table->model()->index(row, MonitorCatalogModel::Object);
        if (index.data().toString() == capturedName) {
            objectIndex = index;
            break;
        }
    }
    ASSERT_TRUE(objectIndex.isValid());
    table->scrollTo(objectIndex);
    QApplication::processEvents();
    const auto sendMouse = [&](QEvent::Type type) {
        const QPoint point = table->visualRect(objectIndex).center();
        QMouseEvent event(type, QPointF(point), QPointF(table->viewport()->mapToGlobal(point)),
                         Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(table->viewport(), &event);
    };
    const auto click = [&] {
        sendMouse(QEvent::MouseButtonPress);
        sendMouse(QEvent::MouseButtonRelease);
    };
    const auto doubleClick = [&] {
        click();
        sendMouse(QEvent::MouseButtonDblClick);
        sendMouse(QEvent::MouseButtonRelease);
    };
    click();
    ASSERT_EQ(located.size(), 1u);
    EXPECT_EQ(located.back(), objectId);
    ASSERT_EQ(table->selectionModel()->selectedRows().size(), 1);
    EXPECT_EQ(table->selectionModel()->selectedRows().front().row(), objectIndex.row());
    panel.copySelected();
    EXPECT_EQ(QApplication::clipboard()->text().split(QLatin1Char('\t')).value(MonitorCatalogModel::Object),
              capturedName);
    doubleClick();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    ASSERT_EQ(opened.size(), 1u);
    EXPECT_EQ(opened.back(), objectId);

    ASSERT_TRUE(projects.renameItem(*selectedSound, capturedName + QStringLiteral("_Renamed")));
    capture.setProjectObjects(ProjectObjectIndex::fromProject(projects.currentProject()));
    QApplication::processEvents();
    EXPECT_EQ(objectIndex.data().toString(), capturedName);
    panel.copySelected();
    EXPECT_EQ(QApplication::clipboard()->text().split(QLatin1Char('\t')).value(MonitorCatalogModel::Object),
              capturedName);

    capture.setProjectObjects({});
    const auto locatedBeforeMissing = located.size();
    const auto openedBeforeMissing = opened.size();
    doubleClick();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_EQ(located.size(), locatedBeforeMissing);
    EXPECT_EQ(opened.size(), openedBeforeMissing);
    EXPECT_EQ(objectIndex.data().toString(), capturedName);

    capture.setProjectObjects(ProjectObjectIndex::fromProject(projects.currentProject()));
    doubleClick();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    ASSERT_GT(located.size(), locatedBeforeMissing);
    EXPECT_EQ(located.back(), objectId);
    ASSERT_EQ(opened.size(), openedBeforeMissing + 1);
    EXPECT_EQ(opened.back(), objectId);

    doubleClick();
    capture.setProjectObjects({});
    const auto openedBeforeInvalidation = opened.size();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_EQ(opened.size(), openedBeforeInvalidation);
}
