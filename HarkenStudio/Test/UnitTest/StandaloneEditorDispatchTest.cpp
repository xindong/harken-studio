#include "Core/ActionManager.h"
#include "Core/AuditionManager.h"
#include "Core/IPC/IPCServer.h"
#include "Core/LayoutController.h"
#include "Core/SelectionRouter.h"
#include "Core/UndoCommands.h"
#include "MainWindow.h"
#include "Monitor/EventCaptureController.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/GridEditor/DiscreteGridWidget.h"
#include "Widgets/GridEditor/SelectionListWidget.h"
#include "Widgets/ControlPanel/ControlPanel.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QAbstractSocket>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTcpSocket>
#include <QMouseEvent>
#include <QTimer>
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

class EditorDockLayoutTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        ASSERT_TRUE(directory_.isValid());
        window_ = std::make_unique<VNX::MainWindow>();
        VNX::ProjectConfig config;
        config.name = QStringLiteral("EditorDockLayout");
        config.location = directory_.path();
        ASSERT_TRUE(pm.createProject(config));
        controller_ = window_->findChild<VNX::LayoutController*>();
        ASSERT_NE(controller_, nullptr);
        controller_->loadProjectLayout();
        QEventLoop startup;
        bool presentationFinished = false;
        bool presentationShown = false;
        QObject::connect(window_.get(), &VNX::MainWindow::startupPresentationFinished,
                         &startup, [&](bool shown) {
            presentationFinished = true;
            presentationShown = shown;
            startup.quit();
        });
        window_->show();
        if (!presentationFinished) {
            QTimer::singleShot(3000, &startup, &QEventLoop::quit);
            startup.exec();
        }
        ASSERT_TRUE(presentationFinished);
        ASSERT_TRUE(presentationShown);
        window_->showNormal();
        window_->resize(1800, 1000);
        drainEvents();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        if (window_)
            QObject::disconnect(&VNX::UndoRedoManager::instance(), nullptr, window_.get(), nullptr);
        window_.reset();
        VNX::ActionManager::instance().setTransactionCallbacks({});
        VNX::ActionManager::instance().clearAllForTesting();
        drainEvents();
    }

    static void drainEvents()
    {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }

    static void clickWidget(QWidget* widget, const QPoint& position,
                            Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QApplication::setActiveWindow(widget->window());
        QMouseEvent press(QEvent::MouseButtonPress, position, widget->mapToGlobal(position),
                          Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, position, widget->mapToGlobal(position),
                            Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(widget, &release);
        drainEvents();
    }

    static void selectChildRow(VNX::GridEditorPanel* panel, int index)
    {
        auto* list = panel->findChild<VNX::SelectionListWidget*>();
        if (list && list->isVisible()) {
            for (auto* row : list->findChildren<QWidget*>(QStringLiteral("selectionChildRow"))) {
                if (row->property("rowIndex").toInt() != index) continue;
                auto* handle = row->findChild<QWidget*>(QStringLiteral("selectionChildDragHandle"));
                ASSERT_NE(handle, nullptr);
                clickWidget(handle, handle->rect().center());
                return;
            }
            FAIL() << "Selection row is missing";
        } else {
            auto* table = panel->discreteGrid()->table();
            ASSERT_NE(table->item(index, 0), nullptr);
            const QRect cell = table->visualItemRect(table->item(index, 0));
            ASSERT_TRUE(table->viewport()->rect().contains(cell.center()));
            clickWidget(table->viewport(), cell.center());
        }
    }

    QTemporaryDir directory_;
    std::unique_ptr<VNX::MainWindow> window_;
    VNX::LayoutController* controller_ = nullptr;
};

}

TEST_F(EditorDockLayoutTest, AuditionTargetInvalidatesWithDeletedSubtreesAndProjectClosure)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* parent = manager.createItem(nullptr, QStringLiteral("AuditionRoot"), VNX::ProjectItem::Type::Container);
    auto* nested = manager.createItem(parent, QStringLiteral("NestedAudition"), VNX::ProjectItem::Type::Container);
    auto* child = manager.createItem(nested, QStringLiteral("NestedChild"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(child, nullptr);
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->onProjectItemDoubleClicked(parent);
    drainEvents();
    auto* panel = qobject_cast<VNX::GridEditorPanel*>(
        window_->activeStandaloneEditorPanel(QStringLiteral("GridEditor")));
    ASSERT_NE(panel, nullptr);
    panel->drillInto(nested->guid);
    drainEvents();
    selectChildRow(panel, 0);
    ASSERT_EQ(window_->auditionTarget(), child);
    ASSERT_TRUE(manager.deleteItem(nested));
    drainEvents();
    EXPECT_EQ(window_->auditionTarget(), nullptr);

    router->selectItem(parent, false);
    ASSERT_EQ(window_->auditionTarget(), parent);
    ASSERT_TRUE(manager.closeProject());
    drainEvents();
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), nullptr);
}

TEST_F(EditorDockLayoutTest, EventIdentityIndexSurvivesObjectEdits)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* variable = manager.createItem(nullptr, QStringLiteral("Intensity"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    VNX::Monitor::CapturedEvent event;
    event.header.stableId = variable->runtimeId();
    event.objectGuid = variable->guid;
    event.projectGuid = QUuid(manager.currentProject()->projectId);
    auto* capture = window_->eventCaptureController();
    ASSERT_NE(capture, nullptr);
    EXPECT_EQ(capture->matchingProjectObject(event), variable->objectID);
    ASSERT_TRUE(manager.renameItem(variable, QStringLiteral("Intensity_Renamed")));
    EXPECT_EQ(capture->matchingProjectObject(event), variable->objectID);
    auto* copy = manager.duplicateItem(variable);
    ASSERT_NE(copy, nullptr);
    EXPECT_NE(copy->runtimeId(), event.header.stableId);
    auto copiedEvent = event;
    copiedEvent.header.stableId = copy->runtimeId();
    copiedEvent.objectGuid = copy->guid;
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), copy->objectID);
    ASSERT_TRUE(manager.deleteItem(variable));
    EXPECT_EQ(capture->matchingProjectObject(event), 0u);
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), copy->objectID);
    copiedEvent.projectGuid = QUuid::createUuid();
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), 0u);
}

TEST(StandaloneEditorIpcEventTest,
     AuditionSeekRequestIdentitySurvivesIpc)
{
    ensureApp();
    auto& projects = VNX::ProjectManager::instance();
    if (projects.isProjectOpen())
        projects.closeProject();
    VNX::ActionManager::instance().clearAllForTesting();

    QTcpSocket socket;
    auto window = std::make_unique<VNX::MainWindow>();
    const auto cleanup = qScopeGuard([&]() {
        socket.abort();
        VNX::UndoRedoManager::instance().clear();
        if (projects.isProjectOpen())
            projects.closeProject();
        QObject::disconnect(&VNX::UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        window.reset();
        VNX::ActionManager::instance().setTransactionCallbacks({});
        VNX::ActionManager::instance().clearAllForTesting();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });

    auto* server = window->ipcServer();
    ASSERT_NE(server, nullptr);
    server->stop();
    ASSERT_TRUE(server->start(0));
    ASSERT_GT(server->port(), 0);

    socket.connectToHost(QHostAddress::LocalHost,
                         static_cast<quint16>(server->port()));
    ASSERT_TRUE(VNX::TestSupport::spinUntil([&]() {
        return socket.state() == QAbstractSocket::ConnectedState
            && server->clientCount() == 1;
    }));

    auto* audition = window->auditionManager();
    ASSERT_NE(audition, nullptr);
    for (const quint64 requestId : {9007199254740993ULL, 9007199254740995ULL}) {
        audition->auditionSeekFinished(
            42, requestId, HarkenEditor::Bridge::kEngineOk, true);
        ASSERT_TRUE(VNX::TestSupport::spinUntil(
            [&]() { return socket.canReadLine(); }));

        QJsonParseError error;
        const QJsonDocument message = QJsonDocument::fromJson(
            socket.readLine().trimmed(), &error);
        ASSERT_EQ(error.error, QJsonParseError::NoError);
        ASSERT_TRUE(message.isObject());
        const QJsonObject event = message.object();
        const QJsonObject data = event.value(QStringLiteral("data")).toObject();
        EXPECT_EQ(data.value(QStringLiteral("audition_seek_request_id")).toString(),
                  QString::number(requestId));
    }
}
