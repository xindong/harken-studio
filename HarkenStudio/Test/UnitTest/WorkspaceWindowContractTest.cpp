#include "WorkspaceWindowTypes.h"
#include "Core/AI/AIAgentConfig.h"
#include "Core/ActionManager.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/NamedLayoutStore.h"
#include "Core/ProjectDocument.h"
#include "Core/ProjectManager.h"
#include "Core/ShortcutManager.h"
#include "Core/UndoCommands.h"
#include "Dialogs/Settings/PreferencesPanel.h"
#include "Dialogs/Settings/ProjectSettingsPanel.h"
#include "MainWindow.h"
#include "Support/AITestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Widgets/Common/StatusTip.h"
#include "Widgets/LogPanel/LogPanel.h"

#include <DockAreaWidget.h>
#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSet>
#include <QStatusBar>
#include <QTemporaryDir>

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

namespace {

class WorkspaceToolLifecycleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(temporary_.isValid());
        ActionManager::instance().clearAllForTesting();
        NamedLayoutStore::setDirPathOverrideForTesting(temporary_.filePath(QStringLiteral("layouts")));
        AIAgentConfigStore::setStorageOverrideForTesting(std::make_shared<MemorySettingsStorage>());
        config_.baseUrl = QStringLiteral("https://saved.example/v1");
        config_.model = QStringLiteral("saved-model");
        AIAgentConfigStore().save(config_);
        window_ = std::make_unique<MainWindow>();
        window_->resize(1200, 800);
        window_->show();
        QApplication::processEvents();
    }

    void TearDown() override
    {
        auto& manager = ProjectManager::instance();
        manager.closeProject();
        EXPECT_TRUE(TestSupport::spinUntil([&] { return !manager.isClosing(); }, 3000));
        QObject::disconnect(&UndoRedoManager::instance(), nullptr, window_.get(), nullptr);
        window_.reset();
        ActionManager::instance().setTransactionCallbacks({});
        ActionManager::instance().clearAllForTesting();
        UndoRedoManager::instance().clear();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        NamedLayoutStore::setDirPathOverrideForTesting({});
        AIAgentConfigStore::setStorageOverrideForTesting(nullptr);
    }

    bool createProject()
    {
        ProjectConfig config;
        config.name = QStringLiteral("WorkspaceLifecycle");
        config.location = temporary_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
        return window_->createProjectFromConfig(config);
    }

    ads::CDockWidget* dock(const QString& type) const
    {
        const auto catalog = defaultWorkspaceWindowCatalog();
        const auto* descriptor = catalog.find(type);
        auto* manager = window_->findChild<ads::CDockManager*>();
        return descriptor && manager ? manager->findDockWidget(descriptor->dockObjectName) : nullptr;
    }

    QTemporaryDir temporary_;
    std::unique_ptr<MainWindow> window_;
    AIAgentConfig config_;
};

}

TEST_F(WorkspaceToolLifecycleTest, ReopeningToolsReusesTheirContentAndDockPlacement)
{
    ASSERT_TRUE(createProject());
    auto* manager = window_->findChild<ads::CDockManager*>();
    auto* anchor = dock(QStringLiteral("Explorer"));
    ASSERT_NE(manager, nullptr);
    ASSERT_NE(anchor, nullptr);
    ASSERT_NE(anchor->dockAreaWidget(), nullptr);
    QPointer<ads::CDockAreaWidget> area = anchor->dockAreaWidget();

    for (const QString& type : {QStringLiteral("ProjectSettings"), QStringLiteral("Preferences"),
                               QStringLiteral("RemoteConnect")}) {
        SCOPED_TRACE(type.toStdString());
        ASSERT_TRUE(window_->setWorkspaceWindowVisible(type, true));
        auto* tool = dock(type);
        ASSERT_NE(tool, nullptr);
        QPointer<QWidget> content = tool->widget();
        ASSERT_FALSE(content.isNull());
        manager->addDockWidget(ads::CenterDockWidgetArea, tool, area);
        for (int opening = 0; opening < 2; ++opening) {
            tool->toggleView(false);
            ASSERT_TRUE(tool->isClosed());
            ASSERT_TRUE(window_->setWorkspaceWindowVisible(type, true));
            EXPECT_EQ(dock(type), tool);
            EXPECT_EQ(tool->widget(), content.data());
            EXPECT_EQ(tool->dockAreaWidget(), area.data());
            EXPECT_FALSE(tool->isInFloatingContainer());
        }
    }
}

TEST_F(WorkspaceToolLifecycleTest, ChangingPreferencePagesKeepsConnectionDraftUntilDockCloses)
{
    ASSERT_TRUE(window_->setWorkspaceWindowVisible(QStringLiteral("Preferences"), true));
    auto* tool = dock(QStringLiteral("Preferences"));
    ASSERT_NE(tool, nullptr);
    auto* panel = qobject_cast<PreferencesPanel*>(tool->widget());
    ASSERT_NE(panel, nullptr);
    auto* page = panel->findChild<AIAgentPage*>();
    auto* url = panel->findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    ASSERT_NE(page, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_TRUE(panel->showPage(QString::fromLatin1(kSettingsPageAIAgent)));
    url->setText(QStringLiteral("https://draft.example/v1"));

    ASSERT_TRUE(panel->showPage(QStringLiteral("appearance")));
    ASSERT_TRUE(panel->showPage(QString::fromLatin1(kSettingsPageAIAgent)));
    EXPECT_TRUE(page->hasPendingChanges());
    EXPECT_EQ(url->text(), QStringLiteral("https://draft.example/v1"));
    EXPECT_EQ(AIAgentConfigStore().effectiveSnapshot().baseUrl, config_.baseUrl);

    tool->toggleView(false);
    EXPECT_FALSE(page->hasPendingChanges());
    ASSERT_TRUE(window_->setWorkspaceWindowVisible(QStringLiteral("Preferences"), true));
    EXPECT_EQ(tool->widget(), panel);
    EXPECT_EQ(url->text(), config_.baseUrl);
}

TEST_F(WorkspaceToolLifecycleTest, RestoringLayoutWithoutAToolClosesItAndEndsItsDraft)
{
    QMainWindow legacyWindow;
    auto* legacyManager = new ads::CDockManager(&legacyWindow);
    auto* existing = new ads::CDockWidget(QStringLiteral("Existing"));
    existing->setObjectName(defaultWorkspaceWindowCatalog().find(QStringLiteral("Explorer"))->dockObjectName);
    existing->setWidget(new QWidget());
    legacyManager->addDockWidget(ads::LeftDockWidgetArea, existing);
    const QString name = QStringLiteral("LegacyWorkspace");
    ASSERT_TRUE(NamedLayoutStore::write(name,
        {{QStringLiteral("name"), name},
         {QStringLiteral("dockState"), QString::fromLatin1(legacyManager->saveState().toBase64())}}));

    ASSERT_TRUE(window_->setWorkspaceWindowVisible(QStringLiteral("Preferences"), true));
    auto* tool = dock(QStringLiteral("Preferences"));
    ASSERT_NE(tool, nullptr);
    auto* panel = qobject_cast<PreferencesPanel*>(tool->widget());
    ASSERT_NE(panel, nullptr);
    auto* page = panel->findChild<AIAgentPage*>();
    auto* url = panel->findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    ASSERT_NE(page, nullptr);
    ASSERT_NE(url, nullptr);
    url->setText(QStringLiteral("https://draft.example/v1"));
    ASSERT_TRUE(page->hasPendingChanges());

    ASSERT_TRUE(window_->loadNamedLayout(name));
    EXPECT_TRUE(tool->isClosed());
    EXPECT_FALSE(page->hasPendingChanges());
    EXPECT_EQ(AIAgentConfigStore().effectiveSnapshot().baseUrl, config_.baseUrl);
    ASSERT_TRUE(window_->setWorkspaceWindowVisible(QStringLiteral("Preferences"), true));
    EXPECT_EQ(tool->widget(), panel);
    EXPECT_EQ(url->text(), config_.baseUrl);
}

TEST_F(WorkspaceToolLifecycleTest, SavingProjectIncludesTheFocusedSettingsEdit)
{
    ASSERT_TRUE(createProject());
    ASSERT_TRUE(window_->setWorkspaceWindowVisible(QStringLiteral("ProjectSettings"), true));
    auto* tool = dock(QStringLiteral("ProjectSettings"));
    ASSERT_NE(tool, nullptr);
    ASSERT_TRUE(tool->isInFloatingContainer());
    auto* panel = qobject_cast<ProjectSettingsPanel*>(tool->widget());
    ASSERT_NE(panel, nullptr);
    ASSERT_TRUE(panel->showPage(QStringLiteral("publish")));
    auto* path = panel->findChild<QLineEdit*>(QStringLiteral("packBuildOutputDirEdit"));
    ASSERT_NE(path, nullptr);
    path->window()->activateWindow();
    path->setFocus();
    ASSERT_TRUE(TestSupport::spinUntil([&] { return path->hasFocus(); }));
    path->selectAll();
    path->insert(QStringLiteral("SavedPacks"));
    ASSERT_TRUE(path->isModified());
    ASSERT_TRUE(ProjectManager::instance().packBuildOutputDir().isEmpty());

    const auto shortcut = ShortcutManager::instance().shortcut(Shortcuts::SaveProject);
    ASSERT_FALSE(shortcut.isEmpty());
    const auto combination = shortcut[0];
    QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers());
    QApplication::sendEvent(path, &press);
    QKeyEvent release(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers());
    QApplication::sendEvent(path, &release);
    EXPECT_EQ(ProjectManager::instance().packBuildOutputDir(), QStringLiteral("SavedPacks"));
    QFile file(ProjectManager::instance().currentProject()->projectPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    ProjectDocumentData document;
    QString error;
    ASSERT_TRUE(ProjectDocument::parse(saved, QStringLiteral("project.root"), &document, &error))
        << error.toStdString();
    EXPECT_EQ(document.content.value(QStringLiteral("packBuildOutputDir")).toString(), QStringLiteral("SavedPacks"));
}

TEST_F(WorkspaceToolLifecycleTest, RestoringCommittedProjectKeepsSavedToolsOpenBeforePublication)
{
    ASSERT_TRUE(createProject());
    auto* dockManager = window_->findChild<ads::CDockManager*>();
    ASSERT_NE(dockManager, nullptr);
    ads::CDockAreaWidget* area = nullptr;
    for (const QString& type : {QStringLiteral("Preferences"), QStringLiteral("ProjectSettings"),
                               QStringLiteral("RemoteConnect")}) {
        ASSERT_TRUE(window_->setWorkspaceWindowVisible(type, true));
        auto* tool = dock(type);
        ASSERT_NE(tool, nullptr);
        area = dockManager->addDockWidget(area ? ads::CenterDockWidgetArea : ads::RightDockWidgetArea,
                                         tool, area);
    }
    area->setCurrentDockWidget(dock(QStringLiteral("RemoteConnect")));
    ASSERT_TRUE(window_->saveNamedLayout(QStringLiteral("StartupWorkspace")));

    auto& manager = ProjectManager::instance();
    const QString path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(TestSupport::spinUntil([&] { return !manager.isClosing(); }, 3000));
    auto* settings = dock(QStringLiteral("ProjectSettings"));
    ASSERT_TRUE(settings->isClosed());
    ASSERT_FALSE(settings->toggleViewAction()->isEnabled());
    auto prepared = ProjectManager::prepareProject(path);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    ASSERT_TRUE(manager.commitPreparedProject(std::move(prepared)));

    window_->finishPreparedProject(path, false);
    for (const QString& type : {QStringLiteral("Preferences"), QStringLiteral("ProjectSettings"),
                               QStringLiteral("RemoteConnect")}) {
        SCOPED_TRACE(type.toStdString());
        EXPECT_FALSE(dock(type)->isClosed());
        EXPECT_EQ(dock(type)->dockAreaWidget(), settings->dockAreaWidget());
    }
    ASSERT_NE(settings->dockAreaWidget(), nullptr);
    EXPECT_EQ(settings->dockAreaWidget()->currentDockWidget(), dock(QStringLiteral("RemoteConnect")));
    EXPECT_TRUE(settings->toggleViewAction()->isEnabled());

    manager.publishProjectOpened();
    EXPECT_FALSE(settings->isClosed());
    EXPECT_TRUE(settings->toggleViewAction()->isEnabled());
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
