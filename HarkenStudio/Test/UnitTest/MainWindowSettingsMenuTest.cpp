#include "ActionManager.h"
#include "MainWindow.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include "Support/AITestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QEvent>
#include <QEventLoop>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

using namespace VNX;

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
    const auto answer = [&](QMessageBox::StandardButton response) {
        QEventLoop events;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("externalProjectChangeDialog")) return;
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
    EXPECT_EQ(manager.findItemByGuid(actionGuid), action);
    EXPECT_EQ(manager.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Pending")), folder);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), history);
    answer(QMessageBox::Yes);
    ASSERT_NE(manager.currentProject(), nullptr);
    EXPECT_EQ(manager.findItemByGuid(actionGuid), nullptr);
    EXPECT_EQ(manager.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Pending")), nullptr);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}
