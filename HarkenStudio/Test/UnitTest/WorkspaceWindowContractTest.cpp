#include "WorkspaceWindowTypes.h"
#include "ActionManager.h"
#include "MainWindow.h"
#include "ProjectManager.h"
#include "UndoCommands.h"
#include "Widgets/Common/StatusTip.h"
#include "Widgets/LogPanel/LogPanel.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QMainWindow>
#include <QPushButton>
#include <QScopeGuard>
#include <QSet>
#include <QStatusBar>

#include <memory>

using namespace VNX;

TEST(EditorWorkspaceWindowContract, DescriptorsHaveUniqueIds)
{
    const WorkspaceWindowCatalog catalog = defaultWorkspaceWindowCatalog();
    QSet<QString> typeIds;
    QSet<QString> dockObjectNames;

    for (const WorkspaceWindowDescriptor& descriptor : catalog.descriptors()) {
        EXPECT_FALSE(descriptor.typeId.isEmpty());
        EXPECT_FALSE(descriptor.dockObjectName.isEmpty());
        EXPECT_FALSE(typeIds.contains(descriptor.typeId));
        EXPECT_FALSE(dockObjectNames.contains(descriptor.dockObjectName));
        typeIds.insert(descriptor.typeId);
        dockObjectNames.insert(descriptor.dockObjectName);
    }
}

TEST(EditorWorkspaceWindowContract, FloatingPanelStatusStaysWithItsOwningWindow)
{
    QMainWindow firstWindow;
    QMainWindow secondWindow;
    auto* firstStatus = firstWindow.statusBar();
    auto* secondStatus = secondWindow.statusBar();
    QWidget firstFloating(&firstWindow, Qt::Window);
    QWidget firstPanel(&firstFloating);
    QWidget secondFloating(&secondWindow, Qt::Window);
    QWidget secondPanel(&secondFloating);

    ASSERT_TRUE(firstFloating.isWindow());
    ASSERT_TRUE(secondFloating.isWindow());
    showStatusTip(&firstPanel, QStringLiteral("First panel feedback"));
    EXPECT_EQ(firstStatus->currentMessage(), QStringLiteral("First panel feedback"));
    EXPECT_TRUE(secondStatus->currentMessage().isEmpty());

    showStatusTip(&secondPanel, QStringLiteral("Second panel feedback"));
    EXPECT_EQ(firstStatus->currentMessage(), QStringLiteral("First panel feedback"));
    EXPECT_EQ(secondStatus->currentMessage(), QStringLiteral("Second panel feedback"));
}

TEST(EditorWorkspaceWindowContract, DiagnosticDetailsDoNotChangeWorkspaceGeometry)
{
    ActionManager::instance().clearAllForTesting();
    auto window = std::make_unique<MainWindow>();
    auto cleanup = qScopeGuard([&] {
        auto& manager = ProjectManager::instance();
        manager.closeProject();
        TestSupport::spinUntil([&] { return !manager.isClosing(); }, 3000);
        QObject::disconnect(&UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        window.reset();
        ActionManager::instance().setTransactionCallbacks({});
        ActionManager::instance().clearAllForTesting();
        UndoRedoManager::instance().clear();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    window->resize(1000, 700);
    window->show();
    auto* button = window->findChild<QPushButton*>(QStringLiteral("statusLogButton"));
    ASSERT_NE(button, nullptr);
    button->click();
    ASSERT_NE(LogPanel::globalModel(), nullptr);
    auto* logText = LogPanel::instance()->findChild<LogTextEdit*>();
    ASSERT_NE(logText, nullptr);
    QApplication::processEvents();

    const QStringList messages{
        QStringLiteral("Output unavailable\n") + QStringLiteral("D:/Audio/Generated/file.bytes\n").repeated(1000),
        QStringLiteral("A diagnostic with a long name: ") + QString(20000, QLatin1Char('x')),
        QStringLiteral("Permissions & locks\r\nDetails\r\n").repeated(1000)};
    for (qsizetype i = 0; i < messages.size(); ++i) {
        const QString ready = QStringLiteral("Ready %1").arg(i);
        LogPanel::logInfo(LogCategory::GUI, ready);
        ASSERT_TRUE(TestSupport::spinUntil([&] {
            return logText->toPlainText().contains(ready + QLatin1Char('\n'));
        }));
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
        const QRect workspace = window->centralWidget()->geometry();
        const QRect status = window->statusBar()->geometry();
        const QSize minimum = window->minimumSizeHint();

        const QString complete = QStringLiteral("Diagnostic complete %1").arg(i);
        const QString message = messages.at(i) + QLatin1Char('\n') + complete;
        LogPanel::logError(LogCategory::GUI, message);
        ASSERT_TRUE(TestSupport::spinUntil([&] {
            return logText->toPlainText().contains(complete + QLatin1Char('\n'));
        }));
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
        EXPECT_EQ(window->centralWidget()->geometry(), workspace);
        EXPECT_EQ(window->statusBar()->geometry(), status);
        EXPECT_EQ(window->minimumSizeHint(), minimum);
        EXPECT_EQ(LogPanel::globalModel()->entries(LogEntry::Error, QStringLiteral("GUI")).back().message, message);
    }
}
