#include "ActionManager.h"
#include "MainWindow.h"
#include "Core/WindowPlacement.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include "Support/AITestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QEventLoop>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QWindow>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

using namespace VNX;

namespace {

QAction* findNamedAction(QWidget* root, const QString& objectName)
{
    for (QAction* action : root->findChildren<QAction*>())
        if (action->objectName() == objectName)
            return action;
    return nullptr;
}

}

TEST(MainWindowSettingsMenu, ProjectSettingsFollowsProjectState)
{
    TestSupport::ensureApp();
    ActionManager::instance().clearAllForTesting();

    auto window = std::make_unique<MainWindow>();
    auto* projectSettings = findNamedAction(window.get(),
        QStringLiteral("fileMenu_projectSettings"));
    ASSERT_NE(projectSettings, nullptr);

    if (ProjectManager::instance().isProjectOpen())
        ProjectManager::instance().closeProject();
    EXPECT_FALSE(projectSettings->isEnabled());

    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    ProjectConfig config;
    config.name = QStringLiteral("MenuSettingsProject");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(QStringList{QStringLiteral("Windows")});
    ASSERT_TRUE(ProjectManager::instance().createProject(config));
    EXPECT_TRUE(projectSettings->isEnabled());

    UndoRedoManager::instance().clear();
    ProjectManager::instance().closeProject();
    EXPECT_FALSE(projectSettings->isEnabled());

    QObject::disconnect(&UndoRedoManager::instance(), nullptr, window.get(), nullptr);
    window.reset();
    ActionManager::instance().setTransactionCallbacks({});
    ActionManager::instance().clearAllForTesting();
    UndoRedoManager::instance().clear();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

TEST(MainWindowSettingsMenu, ExternalChangeChoiceKeepsOrDiscardsTheWholePendingProject)
{
    TestSupport::ensureApp();
    ActionManager::instance().clearAllForTesting();
    auto window = std::make_unique<MainWindow>();
    auto& manager = ProjectManager::instance();
    QTemporaryDir temporary;
    auto cleanup = qScopeGuard([&] {
        manager.closeProject();
        if (manager.isClosing()) {
            QEventLoop completion;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, [&] {
                if (!manager.isClosing()) completion.quit();
            });
            poll.start(1);
            QTimer::singleShot(3000, &completion, &QEventLoop::quit);
            completion.exec();
        }
        QObject::disconnect(&UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        window.reset();
        ActionManager::instance().setTransactionCallbacks({});
        ActionManager::instance().clearAllForTesting();
        UndoRedoManager::instance().clear();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    });
    ASSERT_TRUE(temporary.isValid());
    ProjectConfig config;
    config.name = QStringLiteral("ExternalChangeProject");
    config.location = temporary.path();
    config.platforms = HarkenTest::platforms(QStringList{QStringLiteral("Windows")});
    ASSERT_TRUE(manager.createProject(config));
    auto* folder = manager.createFolder(nullptr, QStringLiteral("Pending"), QStringLiteral("Actions"));
    auto* action = manager.createItem(nullptr, QStringLiteral("PendingAction"), ProjectItem::Type::Action,
                                      QStringLiteral("Pending"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(action, nullptr);
    const QUuid actionGuid = action->guid;
    const int history = UndoRedoManager::instance().undoStack()->count();
    int prompts = 0;
    const auto answer = [&](QMessageBox::StandardButton response) {
        QEventLoop events;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("externalProjectChangeDialog")) return;
            ++prompts;
            dialog->button(response)->click();
            if (response == QMessageBox::No) events.quit();
        });
        QObject::connect(&manager, &ProjectManager::projectOpened, &events, &QEventLoop::quit);
        responder.start(1);
        emit manager.packExternallyModified(QStringLiteral("Actions/Changed.vp"));
        emit manager.packExternallyModified(QStringLiteral("Actions/NewFolder"));
        emit manager.packExternallyModified(QStringLiteral("ExternalChangeProject.hsp"));
        QTimer::singleShot(3000, &events, &QEventLoop::quit);
        events.exec();
    };
    answer(QMessageBox::No);
    EXPECT_EQ(prompts, 1);
    EXPECT_EQ(manager.findItemByGuid(actionGuid), action);
    EXPECT_EQ(manager.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Pending")), folder);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), history);
    answer(QMessageBox::Yes);
    EXPECT_EQ(prompts, 2);
    ASSERT_NE(manager.currentProject(), nullptr);
    EXPECT_EQ(manager.findItemByGuid(actionGuid), nullptr);
    EXPECT_EQ(manager.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Pending")), nullptr);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

namespace {

class MainWindowPlacement : public testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ActionManager::instance().clearAllForTesting();
        QSettings settings;
        settings.remove("MainWindow");
    }

    void destroyWindow()
    {
        if (window_) {
            QObject::disconnect(&UndoRedoManager::instance(), nullptr, window_.get(), nullptr);
            window_.reset();
        }
        ActionManager::instance().setTransactionCallbacks({});
        ActionManager::instance().clearAllForTesting();
        UndoRedoManager::instance().clear();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }

    void TearDown() override
    {
        destroyWindow();
        QSettings settings;
        settings.remove("MainWindow");
    }

    void settle()
    {
        QEventLoop loop;
        QTimer::singleShot(30, &loop, &QEventLoop::quit);
        loop.exec();
    }

    void seed(const QSize& size, Qt::WindowState state)
    {
        QSettings settings;
        settings.beginGroup("MainWindow");
        settings.setValue("normalSize", size);
        settings.setValue("presentationState", int(state));
    }

    void expectCentered()
    {
        const QRect available = window_->screen()->availableGeometry();
        const QRect frame = window_->frameGeometry();
        EXPECT_LE(qAbs(frame.center().x() - available.center().x()), 1);
        EXPECT_LE(qAbs(frame.center().y() - available.center().y()), 1);
    }

    void roundTrip(Qt::WindowState mode, bool minimized = false)
    {
        QScreen* target = QGuiApplication::screens().last();
        const QRect available = target->availableGeometry();
        if (available.width() < 1200 || available.height() < 750)
            GTEST_SKIP() << "Round-trip size requires a larger native work area";
        QSize expected(1100, 650);
        seed(expected, mode);
        for (int cycle = 0; cycle < 2; ++cycle) {
            window_ = std::make_unique<MainWindow>();
            window_->prepareWindowForStartup(false, target);
            window_->show();
            settle();
            EXPECT_EQ(persistentWindowState(window_->windowState()), mode);
            EXPECT_EQ(window_->screen(), target);
            window_->showNormal();
            settle();
            EXPECT_EQ(window_->size(), expected);
            if (cycle == 0) {
                expected = QSize(1120, 670);
                window_->resize(expected);
                settle();
            }
            window_->setWindowState(mode);
            settle();
            if (minimized) {
                window_->showMinimized();
                settle();
            }
            ASSERT_TRUE(window_->close());
            QSettings settings;
            settings.beginGroup("MainWindow");
            EXPECT_EQ(settings.value("normalSize").toSize(), expected);
            EXPECT_EQ(settings.value("presentationState").toInt(), int(mode));
            EXPECT_FALSE(settings.contains("geometry"));
            destroyWindow();
        }
    }

    void watchPresentation()
    {
        QObject::connect(window_.get(), &MainWindow::startupPresentationFinished,
                         window_.get(), [this](bool shown) { presentations_.append(shown); });
    }

    QList<bool> presentations_;
    std::unique_ptr<MainWindow> window_;
};

}

TEST_F(MainWindowPlacement, NewProjectStartsAtDefaultSizeWithoutInheritedState)
{
    seed({900, 620}, Qt::WindowMaximized);
    window_ = std::make_unique<MainWindow>();
    window_->prepareWindowForStartup(true, QGuiApplication::primaryScreen());
    window_->show();
    settle();
    const QRect available = window_->screen()->availableGeometry();
    if (!window_->isMaximized()) {
        expectCentered();
        EXPECT_GE(window_->width(), qMin(available.width() - 16, kDefaultStudioWindowSize.width()));
    }
    EXPECT_LE(window_->width(), qMax(available.width(), window_->minimumWidth()));
}

TEST_F(MainWindowPlacement, ReopenedWindowCentersAndDoesNotRecenterOnLaterShow)
{
    seed({1100, 650}, Qt::WindowNoState);
    window_ = std::make_unique<MainWindow>();
    window_->prepareWindowForStartup(false, QGuiApplication::primaryScreen());
    window_->show();
    settle();
    if (window_->isMaximized())
        GTEST_SKIP() << "Current work area cannot fit a normal main window";
    expectCentered();
    const QPoint position = window_->pos() + QPoint(17, 11);
    window_->move(position);
    settle();
    const QPoint movedPosition = window_->pos();
    window_->hide();
    window_->show();
    settle();
    EXPECT_LE(qAbs(window_->pos().x() - movedPosition.x()), 1);
    EXPECT_LE(qAbs(window_->pos().y() - movedPosition.y()), 1);
    window_->close();
    QSettings settings;
    settings.beginGroup("MainWindow");
    EXPECT_FALSE(settings.contains("geometry"));
    EXPECT_EQ(settings.value("normalSize").toSize(), window_->size());
}

TEST_F(MainWindowPlacement, NormalCloseKeepsTheLastResizedClientSize)
{
    roundTrip(Qt::WindowNoState);
}

TEST_F(MainWindowPlacement, MaximizedCloseKeepsNormalSizeAcrossTwoReopens)
{
    roundTrip(Qt::WindowMaximized);
}

TEST_F(MainWindowPlacement, FullScreenCloseKeepsNormalSizeAcrossTwoReopens)
{
    roundTrip(Qt::WindowFullScreen);
}

TEST_F(MainWindowPlacement, MinimizedCloseKeepsTheUnderlyingMaximizedPreference)
{
    roundTrip(Qt::WindowMaximized, true);
}

TEST_F(MainWindowPlacement, DirectShowPreparesCenteredPresentationOnlyOnce)
{
    seed({1100, 650}, Qt::WindowNoState);
    window_ = std::make_unique<MainWindow>();
    window_->move(100000, 100000);
    window_->show();
    settle();
    if (!window_->isMaximized())
        expectCentered();
    window_->showMaximized();
    settle();
    EXPECT_TRUE(window_->isMaximized());
}

TEST_F(MainWindowPlacement, PresentationFinishesOnceAndLaterShowsDoNotRestartIt)
{
    seed({1100, 650}, Qt::WindowNoState);
    window_ = std::make_unique<MainWindow>();
    watchPresentation();
    window_->prepareWindowForStartup(false, nullptr);
    window_->show();
    settle();
    EXPECT_EQ(presentations_, (QList<bool>{true}));
    window_->hide();
    window_->show();
    settle();
    EXPECT_EQ(presentations_, (QList<bool>{true}));
}

TEST_F(MainWindowPlacement, ClosingBeforeFirstExposureFinishesStartupAsCancelled)
{
    window_ = std::make_unique<MainWindow>();
    watchPresentation();
    window_->prepareWindowForStartup(false, nullptr);
    ASSERT_TRUE(window_->close());
    settle();
    EXPECT_EQ(presentations_, (QList<bool>{false}));
}

TEST_F(MainWindowPlacement, HidingBeforePresentationDoesNotWaitForExposure)
{
    window_ = std::make_unique<MainWindow>();
    watchPresentation();
    window_->prepareWindowForStartup(false, nullptr);
    window_->show();
    window_->hide();
    settle();
    EXPECT_EQ(presentations_, (QList<bool>{false}));
}

TEST_F(MainWindowPlacement, UserMinimizingBeforeExposureKeepsTheirStateAndCompletes)
{
    window_ = std::make_unique<MainWindow>();
    watchPresentation();
    window_->prepareWindowForStartup(false, nullptr);
    window_->showMinimized();
    settle();
    EXPECT_TRUE(window_->isMinimized());
    EXPECT_EQ(presentations_, (QList<bool>{true}));
}

TEST_F(MainWindowPlacement, MinimumSizeLargerThanWorkAreaUsesMaximizedPresentation)
{
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        GTEST_SKIP() << "Native Windows maximization required";
    window_ = std::make_unique<MainWindow>();
    QScreen* screen = QGuiApplication::screens().last();
    const QRect available = screen->availableGeometry();
    window_->setMinimumSize(available.size() + QSize(100, 100));
    watchPresentation();
    window_->prepareWindowForStartup(true, screen);
    window_->show();
    settle();
    EXPECT_TRUE(window_->isMaximized());
    EXPECT_EQ(window_->screen(), screen);
    EXPECT_TRUE(available.contains(window_->geometry()));
    EXPECT_EQ(presentations_, (QList<bool>{true}));
}
