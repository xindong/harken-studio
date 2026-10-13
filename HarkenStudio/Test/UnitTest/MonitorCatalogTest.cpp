#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Widgets/Monitor/MonitorCatalogPanel.h"
#include "ActionManager.h"
#include "Core/AudioEngine.h"
#include "Core/ProjectManager.h"
#include "Core/RuntimeSyncCoordinator.h"
#include "Core/ToastManager.h"
#include "Core/UndoCommands.h"
#include "Core/WorkspaceWindowManager.h"
#include "MainWindow.h"
#include "Widgets/PlatformBar/PlatformBar.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QElapsedTimer>
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

TEST(MonitorCatalog, LocalCaptureKeepsHistoryAcrossPanelLifetime)
{
    ensureApp();
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    Monitor::EventCaptureController capture(nullptr);
    capture.setLocalRuntimeAvailable(true);
    auto panel = std::make_unique<MonitorCatalogPanel>();
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
    ASSERT_EQ(VN_OK, Vn_StopAll(19));
    ASSERT_TRUE(waitFor([&] { return panel->visibleRowCount() == 1; }));
    EXPECT_TRUE(panel->table()->model()->index(0, 4).data().toString().contains(QStringLiteral("19 ms")));
    panel->table()->selectRow(0);
    QPersistentModelIndex selected(panel->table()->currentIndex());
    int resets = 0;
    QObject::connect(panel->table()->model(), &QAbstractItemModel::modelReset, panel.get(), [&] { ++resets; });
    ASSERT_EQ(VN_OK, Vn_StopAll(23));
    ASSERT_TRUE(waitFor([&] { return panel->visibleRowCount() == 2; }));
    EXPECT_TRUE(selected.isValid());
    EXPECT_EQ(selected, panel->table()->currentIndex());
    EXPECT_EQ(resets, 0);
    panel.reset();
    ASSERT_EQ(VN_OK, Vn_StopAll(31));
    ASSERT_TRUE(waitFor([&] { return capture.rowCount() == 3; }));
    panel = std::make_unique<MonitorCatalogPanel>();
    panel->setCaptureController(&capture);
    EXPECT_EQ(panel->visibleRowCount(), 3);
    capture.stop();
    ASSERT_TRUE(waitFor([&] { return capture.state() == Monitor::EventCaptureState::Stopped; }));
    EXPECT_EQ(panel->visibleRowCount(), 3);
    ASSERT_EQ(VN_OK, Vn_StopAll(37));
    ASSERT_EQ(VN_OK, Vn_Update());
    capture.pollLocal();
    EXPECT_EQ(panel->visibleRowCount(), 3);
    capture.start();
    EXPECT_EQ(panel->visibleRowCount(), 0);
    ASSERT_EQ(VN_OK, Vn_StopAll(41));
    ASSERT_TRUE(waitFor([&] { return panel->visibleRowCount() == 1; }));
    panel->findChild<QToolButton*>(QStringLiteral("MonitorCatalogClear"))->click();
    EXPECT_EQ(capture.state(), Monitor::EventCaptureState::Capturing);
    EXPECT_EQ(capture.rowCount(), 0);
    ASSERT_EQ(VN_OK, Vn_StopAll(43));
    ASSERT_TRUE(waitFor([&] { return panel->visibleRowCount() == 1; }));
    EXPECT_TRUE(panel->table()->model()->index(0, 4).data().toString().contains(QStringLiteral("43 ms")));
}

TEST(MonitorCatalog, LiveParamsRestoreCategoryLevelAndScrolling)
{
    ensureApp();
    Monitor::EventCaptureController capture(nullptr);
    MonitorCatalogPanel panel;
    panel.setCaptureController(&capture);
    QVariantMap params{
        {QStringLiteral("filterCategory"), VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC},
        {QStringLiteral("filterLevel"), VN_LOG_ERROR},
        {QStringLiteral("filterText"), QStringLiteral("overload")},
        {QStringLiteral("autoScroll"), false},
    };
    panel.restoreParams(params);
    const auto restored = panel.snapshotParams();
    for (auto it = params.cbegin(); it != params.cend(); ++it) EXPECT_EQ(restored.value(it.key()), it.value());
    auto* category = panel.findChild<QComboBox*>(QStringLiteral("MonitorCatalogCategoryFilter"));
    ASSERT_NE(category, nullptr);
    EXPECT_EQ(category->currentData().toInt(), VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC);
    panel.restoreParams({{QStringLiteral("filterType"), QStringLiteral("All")}});
    EXPECT_EQ(category->currentIndex(), 0);
    panel.restoreParams({{QStringLiteral("filterCategory"), 99}});
    EXPECT_EQ(category->currentIndex(), 0);
}

TEST(MonitorCatalog, ToolbarCapturesCurrentProjectAcrossPanelAndLayoutChanges)
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
    QTemporaryDir directory(QStringLiteral(HARKEN_TEMP_DIR "/event-toolbar-XXXXXX"));
    ASSERT_TRUE(directory.isValid());
    auto window = std::make_unique<MainWindow>();
    const auto cleanup = qScopeGuard([&] {
        projects.closeProject();
        QObject::disconnect(&UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        ToastManager::instance().setParentWidget(nullptr);
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
    auto* button = window->findChild<QToolButton*>(QStringLiteral("monitorCaptureButton"));
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
    auto* catalog = view->findChild<QAction*>(QStringLiteral("viewToggle_MonitorCatalogDock"));
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
    auto* idlePanel = window->findChild<MonitorCatalogPanel*>();
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
    const auto waitFor = [&](const auto& ready) {
        QElapsedTimer timer;
        timer.start();
        while (!ready() && timer.elapsed() < 3000) {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
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
    auto* dock = window->findChild<ads::CDockWidget*>(QStringLiteral("MonitorCatalogDock"));
    ASSERT_NE(dock, nullptr);
    dock->toggleView(false);
    QApplication::processEvents();
    ASSERT_TRUE(dock->isClosed());
    ASSERT_FALSE(idlePanel->isVisible());
    button->click();
    QApplication::processEvents();
    ASSERT_EQ(capture->state(), Monitor::EventCaptureState::Capturing);
    EXPECT_TRUE(button->isChecked());
    auto* panel = window->findChild<MonitorCatalogPanel*>();
    ASSERT_NE(panel, nullptr);
    EXPECT_TRUE(dock->isClosed());
    EXPECT_FALSE(panel->isVisible());
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ToRuntime(variable->runtimeId()), 0.25f));
    ASSERT_TRUE(waitFor([&] { return capture->rowCount() > 0; }));
    const auto matchingRow = [&] {
        for (int i = 0; i < capture->rowCount(); ++i)
            if (capture->eventAt(i).header.stableId == variable->runtimeId()) return i;
        return -1;
    };
    ASSERT_TRUE(waitFor([&] { return matchingRow() >= 0; }));
    const int row = matchingRow();
    EXPECT_EQ(capture->eventAt(row).objectName, QStringLiteral("Intensity"));
    EXPECT_EQ(capture->matchingProjectObject(capture->eventAt(row)), variable->objectID);
    EXPECT_TRUE(dock->isClosed());
    EXPECT_FALSE(panel->isVisible());
    ASSERT_TRUE(window->setWorkspaceWindowVisible(QStringLiteral("MonitorCatalog"), true));
    QApplication::processEvents();
    EXPECT_FALSE(dock->isClosed());
    EXPECT_TRUE(panel->isVisible());
    EXPECT_FALSE(panel->visibleRegion().isEmpty());
    EXPECT_EQ(panel->visibleRowCount(), capture->rowCount());
    panel->table()->selectRow(row);
    EXPECT_TRUE(panel->jumpCurrentRow());
    ASSERT_TRUE(projects.renameItem(variable, QStringLiteral("Drive")));
    EXPECT_EQ(capture->eventAt(row).objectName, QStringLiteral("Intensity"));

    const QRect savedFrame = panel->window()->frameGeometry();
    ASSERT_TRUE(window->saveNamedLayout(QStringLiteral("MonitorCatalogTest")));
    dock->toggleView(false);
    EXPECT_EQ(capture->state(), Monitor::EventCaptureState::Capturing);
    const auto count = capture->rowCount();
    ASSERT_EQ(VN_OK, Vn_StopAll(17));
    ASSERT_TRUE(waitFor([&] { return capture->rowCount() > count; }));
    ASSERT_TRUE(window->loadNamedLayout(QStringLiteral("MonitorCatalogTest")));
    EXPECT_FALSE(dock->isClosed());
    EXPECT_TRUE(panel->isVisible());
    EXPECT_EQ(panel->window()->frameGeometry(), savedFrame);
    EXPECT_EQ(panel->visibleRowCount(), capture->rowCount());
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
    ASSERT_TRUE(window->setWorkspaceWindowVisible(QStringLiteral("MonitorCatalog"), true));
    QApplication::processEvents();
    EXPECT_EQ(panel->window()->frameGeometry(), userFrame);
    panel->window()->move(available.bottomRight() + QPoint(100, 100));
    dock->toggleView(false);
    view->popup(window->mapToGlobal(QPoint(20, 40)));
    QApplication::processEvents();
    catalog = view->findChild<QAction*>(QStringLiteral("viewToggle_MonitorCatalogDock"));
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

TEST(MonitorCatalog, RestoredCatalogIsVisibleBeforeCaptureAndCanReopen)
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
            workspace.registerSingletonFactory(QStringLiteral("MonitorCatalog"), {
                [&]() -> QWidget* {
                    auto* panel = new MonitorCatalogPanel();
                    panel->setCaptureController(&capture);
                    return panel;
                }, {}, {}});
            auto* dock = workspace.ensureSingleton(QStringLiteral("MonitorCatalog"));
            ASSERT_NE(dock, nullptr);
            window.show();
            QApplication::processEvents();
            if (restoring) {
                ASSERT_TRUE(docks->restoreState(savedState));
                EXPECT_EQ(workspace.materializeVisibleSingletons(), 1);
            } else {
                ASSERT_TRUE(workspace.showSingleton(QStringLiteral("MonitorCatalog")));
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
            ASSERT_TRUE(workspace.showSingleton(QStringLiteral("MonitorCatalog")));
            QApplication::processEvents();
            EXPECT_TRUE(dock->widget()->isVisible());
            ASSERT_TRUE(workspace.hideSingleton(QStringLiteral("MonitorCatalog")));
            ASSERT_TRUE(workspace.showSingleton(QStringLiteral("MonitorCatalog")));
            QApplication::processEvents();
            EXPECT_TRUE(dock->widget()->isVisible());
        }
    }
}
