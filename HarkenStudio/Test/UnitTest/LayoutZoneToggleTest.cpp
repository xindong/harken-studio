#include "Core/LayoutController.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Core/BuiltinTheme.h"
#include "Core/NamedLayoutStore.h"
#include "Core/QtAdsThemeAdapter.h"
#include "Core/WorkspaceWindowManager.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockSplitter.h>
#include <DockWidget.h>

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QTemporaryDir>

#include <gtest/gtest.h>

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

ads::CDockWidget* makeDock(const QString& objectName)
{
    auto* dock = new ads::CDockWidget(objectName);
    dock->setObjectName(objectName);
    dock->setWidget(new QLabel(objectName));
    return dock;
}

struct ZoneFixture
{
    QTemporaryDir projectDir;
    std::unique_ptr<QMainWindow> window;
    ads::CDockManager* manager = nullptr;
    ads::CDockWidget* canvas = nullptr;
    ads::CDockWidget* ai = nullptr;
    ads::CDockWidget* projectExplorer = nullptr;
    ads::CDockWidget* audition = nullptr;
    ads::CDockWidget* information = nullptr;
    std::unique_ptr<VNX::LayoutController> controller;

    ZoneFixture()
    {
        ensureApp();
        VNX::AssetRegistry::instance().setProjectRoot(projectDir.path());

        window = std::make_unique<QMainWindow>();
        window->resize(1400, 900);
        manager = new ads::CDockManager(window.get());

        canvas          = makeDock(QStringLiteral("BlueprintDock"));
        ai              = makeDock(QStringLiteral("AssistantDock"));
        projectExplorer = makeDock(QStringLiteral("ExplorerDock"));
        audition       = makeDock(QStringLiteral("ControlDock"));
        information     = makeDock(QStringLiteral("InformationDock"));

        controller = makeController();
        controller->applyDefaultLayout();
        window->show();
        QApplication::processEvents();
    }

    ~ZoneFixture()
    {
        window->hide();
        QApplication::processEvents();
    }

    std::unique_ptr<VNX::LayoutController> makeController()
    {
        auto made = std::make_unique<VNX::LayoutController>(nullptr);

        VNX::LayoutController::Docks docks;
        docks.dockManager          = manager;
        docks.blueprintDock  = canvas;
        docks.projectExplorerDock  = projectExplorer;
        docks.assistantDock               = ai;
        docks.controlDock = audition;
        made->setDocks(docks);
        made->setWindowDockResolver(
            [this](const QString& typeId) -> ads::CDockWidget* {
                return typeId == QLatin1String("Information") ? information : nullptr;
            });
        return made;
    }

    void reopenFromCache()
    {
        controller = makeController();
        controller->loadProjectLayout();
        QApplication::processEvents();
    }

    QJsonObject cachedLayout() const
    {
        QFile file(VNX::PathUtils::getDotCacheDir(projectDir.path())
                   + QStringLiteral("/editor_layout.json"));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        return json;
    }

    ads::CDockSplitter* leftColumnSplitter() const
    {
        ads::CDockAreaWidget* area = projectExplorer->dockAreaWidget();
        return area ? area->parentSplitter() : nullptr;
    }

    ads::CDockSplitter* columnRowSplitter() const
    {
        ads::CDockSplitter* column = leftColumnSplitter();
        return column ? qobject_cast<ads::CDockSplitter*>(column->parentWidget()) : nullptr;
    }
};

TEST(LayoutZoneToggleTest, CollapsingLeftZoneHidesEveryDockInTheColumn)
{
    ZoneFixture fixture;
    ASSERT_FALSE(fixture.projectExplorer->isClosed());
    ASSERT_FALSE(fixture.information->isClosed());

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);

    EXPECT_TRUE(fixture.projectExplorer->isClosed());
    EXPECT_TRUE(fixture.information->isClosed());
    EXPECT_FALSE(fixture.canvas->isClosed());
    EXPECT_FALSE(fixture.ai->isClosed());
}

TEST(LayoutZoneToggleTest, SingletonContentStateRestoresWithoutReplacingWindows)
{
    ZoneFixture fixture;
    VNX::WorkspaceWindowManager workspace(VNX::defaultWorkspaceWindowCatalog(),
                                          VNX::QtAdsWindowAdapter());
    VNX::WorkspaceWindowManager::SingletonFactory factory;
    factory.snapshotState = [](QWidget* content) {
        return content->property("workspaceState").toMap();
    };
    factory.restoreState = [](QWidget* content, const QVariantMap& state) {
        content->setProperty("workspaceState", state);
    };
    workspace.registerSingletonFactory(QStringLiteral("Control"), factory);
    workspace.registerSingletonFactory(QStringLiteral("Information"), factory);
    ASSERT_TRUE(workspace.registerSingleton(QStringLiteral("Control"), fixture.audition));
    ASSERT_TRUE(workspace.registerSingleton(QStringLiteral("Information"), fixture.information));
    fixture.controller->setSingletonSnapshotProvider([&] { return workspace.singletonStates(); });
    fixture.controller->setSingletonStateRestorer(
        [&](const QVariantMap& states) { workspace.restoreSingletonStates(states); });

    QWidget* first = fixture.audition->widget();
    QWidget* second = fixture.information->widget();
    const QVariantMap firstState{{QStringLiteral("context"), QStringLiteral("first")}};
    const QVariantMap secondState{{QStringLiteral("context"), QStringLiteral("second")}};
    first->setProperty("workspaceState", firstState);
    second->setProperty("workspaceState", secondState);
    fixture.controller->saveProjectLayout();
    first->setProperty("workspaceState", QVariantMap());
    second->setProperty("workspaceState", QVariantMap());
    fixture.controller->loadProjectLayout();

    EXPECT_EQ(fixture.audition->widget(), first);
    EXPECT_EQ(fixture.information->widget(), second);
    EXPECT_EQ(first->property("workspaceState").toMap(), firstState);
    EXPECT_EQ(second->property("workspaceState").toMap(), secondState);
    EXPECT_TRUE(workspace.instanceSpecs().isEmpty());

    QVariantMap legacyStates = workspace.singletonStates();
    legacyStates.insert(QStringLiteral("Preview"), legacyStates.take(QStringLiteral("Control")));
    first->setProperty("workspaceState", QVariantMap());
    second->setProperty("workspaceState", QVariantMap());
    workspace.restoreSingletonStates(legacyStates);
    EXPECT_EQ(first->property("workspaceState").toMap(), firstState);
    EXPECT_EQ(second->property("workspaceState").toMap(), secondState);

    legacyStates.insert(QStringLiteral("Control"), QVariantMap());
    workspace.restoreSingletonStates(legacyStates);
    EXPECT_TRUE(first->property("workspaceState").toMap().isEmpty());
    EXPECT_EQ(second->property("workspaceState").toMap(), secondState);

    fixture.controller->selectDefaultLayout();
    EXPECT_TRUE(first->property("workspaceState").toMap().isEmpty());
    EXPECT_TRUE(second->property("workspaceState").toMap().isEmpty());
}

TEST(LayoutZoneToggleTest, ExpandingLeftZoneRestoresEveryDockItHid)
{
    ZoneFixture fixture;

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
    fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);

    EXPECT_FALSE(fixture.projectExplorer->isClosed());
    EXPECT_FALSE(fixture.information->isClosed());
}

TEST(LayoutZoneToggleTest, ExpandingRestoresUserChosenProportionsNotTheSeededDefault)
{
    ZoneFixture fixture;
    ads::CDockSplitter* column = fixture.leftColumnSplitter();
    ads::CDockSplitter* row = fixture.columnRowSplitter();
    ASSERT_NE(column, nullptr);
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(column->count(), 2);

    QList<int> columnSizes = column->sizes();
    const int columnTotal = columnSizes.at(0) + columnSizes.at(1);
    column->setSizes({columnTotal * 6 / 10, columnTotal - columnTotal * 6 / 10});

    QList<int> rowSizes = row->sizes();
    ASSERT_GE(rowSizes.size(), 2);
    rowSizes[0] += 140;
    rowSizes[1] -= 140;
    row->setSizes(rowSizes);
    QApplication::processEvents();

    const QList<int> chosenColumn = column->sizes();
    const QList<int> chosenRow = row->sizes();

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
    QApplication::processEvents();
    fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);
    QApplication::processEvents();

    EXPECT_EQ(column->sizes(), chosenColumn);
    EXPECT_EQ(row->sizes().at(0), chosenRow.at(0));
}

TEST(LayoutZoneToggleTest, ExpandingKeepsSidebarWidthAfterTheWindowChangedWhileCollapsed)
{
    ZoneFixture fixture;
    ads::CDockSplitter* row = fixture.columnRowSplitter();
    ASSERT_NE(row, nullptr);
    ASSERT_GE(row->sizes().size(), 2);

    const int startWidth = 1400;
    const int shrunkWidth = 1300;
    const int sidebar = row->sizes().at(0);

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
    QApplication::processEvents();
    fixture.window->resize(shrunkWidth, 900);
    QApplication::processEvents();
    fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);
    QApplication::processEvents();

    const int restored = row->sizes().at(0);
    const int proportional = sidebar * shrunkWidth / startWidth;

    EXPECT_EQ(restored, sidebar);
    EXPECT_GT(restored, proportional + 8);
}

TEST(LayoutZoneToggleTest, RepeatedTogglingDoesNotDriftTheSidebarWidth)
{
    ZoneFixture fixture;
    ads::CDockSplitter* row = fixture.columnRowSplitter();
    ASSERT_NE(row, nullptr);
    const QList<int> before = row->sizes();

    for (int i = 0; i < 5; ++i) {
        fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
        QApplication::processEvents();
        fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);
        QApplication::processEvents();
    }

    EXPECT_EQ(row->sizes(), before);
}

TEST(LayoutZoneToggleTest, ExpandingDoesNotReopenDocksTheUserHadAlreadyClosed)
{
    ZoneFixture fixture;
    fixture.information->toggleView(false);
    ASSERT_TRUE(fixture.information->isClosed());

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
    fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);

    EXPECT_FALSE(fixture.projectExplorer->isClosed());
    EXPECT_TRUE(fixture.information->isClosed());
}

TEST(LayoutZoneToggleTest, ExpandingWithoutASnapshotReopensTheWholeColumn)
{
    ZoneFixture fixture;
    fixture.projectExplorer->toggleView(false);
    fixture.information->toggleView(false);
    QApplication::processEvents();

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);
    QApplication::processEvents();

    EXPECT_FALSE(fixture.projectExplorer->isClosed());
    EXPECT_FALSE(fixture.information->isClosed());
}

TEST(LayoutZoneToggleTest, ACachedCollapseSurvivesReopeningTheProject)
{
    ZoneFixture fixture;

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
    QApplication::processEvents();
    fixture.controller->saveProjectLayout();

    fixture.reopenFromCache();

    EXPECT_TRUE(fixture.projectExplorer->isClosed());
    EXPECT_TRUE(fixture.information->isClosed());
    EXPECT_FALSE(fixture.canvas->isClosed());
}

TEST(LayoutZoneToggleTest, ACachedCollapseReopensOnlyTheDocksItHid)
{
    ZoneFixture fixture;
    fixture.information->toggleView(false);
    QApplication::processEvents();

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
    QApplication::processEvents();
    fixture.controller->saveProjectLayout();

    fixture.reopenFromCache();
    ASSERT_TRUE(fixture.projectExplorer->isClosed());

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);
    QApplication::processEvents();

    EXPECT_FALSE(fixture.projectExplorer->isClosed());
    EXPECT_TRUE(fixture.information->isClosed());
}

TEST(LayoutZoneToggleTest, ACachedCollapseRestoresTheChosenProportions)
{
    ZoneFixture fixture;
    ads::CDockSplitter* row = fixture.columnRowSplitter();
    ads::CDockSplitter* column = fixture.leftColumnSplitter();
    ASSERT_NE(row, nullptr);
    ASSERT_NE(column, nullptr);
    ASSERT_GE(row->sizes().size(), 2);
    ASSERT_EQ(column->count(), 2);

    QList<int> rowSizes = row->sizes();
    rowSizes[0] += 140;
    rowSizes[1] -= 140;
    row->setSizes(rowSizes);

    QList<int> columnSizes = column->sizes();
    columnSizes[0] += 120;
    columnSizes[1] -= 120;
    column->setSizes(columnSizes);
    QApplication::processEvents();

    const int chosenSidebar = row->sizes().at(0);
    const int chosenExplorer = column->sizes().at(0);

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
    QApplication::processEvents();
    fixture.controller->saveProjectLayout();

    fixture.reopenFromCache();
    fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);
    QApplication::processEvents();

    ads::CDockSplitter* reopenedRow = fixture.columnRowSplitter();
    ads::CDockSplitter* reopenedColumn = fixture.leftColumnSplitter();
    ASSERT_NE(reopenedRow, nullptr);
    ASSERT_NE(reopenedColumn, nullptr);

    EXPECT_EQ(reopenedRow->sizes().at(0), chosenSidebar);
    EXPECT_EQ(reopenedColumn->sizes().at(0), chosenExplorer);
}

struct NamedLayoutGuard
{
    QString name;
    ~NamedLayoutGuard() { VNX::NamedLayoutStore::remove(name); }
};

TEST(LayoutZoneToggleTest, ANamedLayoutCarriesTheCollapseItWasSavedWith)
{
    ZoneFixture fixture;
    NamedLayoutGuard guard{QStringLiteral("ZoneToggleTestLayout")};

    fixture.information->toggleView(false);
    QApplication::processEvents();
    fixture.controller->setZoneCollapsed(fixture.projectExplorer, true);
    QApplication::processEvents();
    fixture.controller->saveProjectLayout();

    const QJsonObject cached = fixture.cachedLayout();
    ASSERT_FALSE(cached.value(QStringLiteral("collapsedZones")).toArray().isEmpty());

    QJsonObject named;
    named[QStringLiteral("name")] = guard.name;
    named[QStringLiteral("dockState")] = cached.value(QStringLiteral("state"));
    named[QStringLiteral("collapsedZones")] = cached.value(QStringLiteral("collapsedZones"));
    ASSERT_TRUE(VNX::NamedLayoutStore::write(guard.name, named));

    fixture.controller->selectDefaultLayout();
    QApplication::processEvents();
    ASSERT_FALSE(fixture.projectExplorer->isClosed());
    ASSERT_FALSE(fixture.information->isClosed());

    fixture.controller->onLoadNamedLayout(guard.name);
    QApplication::processEvents();
    EXPECT_TRUE(fixture.projectExplorer->isClosed());
    EXPECT_TRUE(fixture.information->isClosed());

    fixture.controller->setZoneCollapsed(fixture.projectExplorer, false);
    QApplication::processEvents();
    EXPECT_FALSE(fixture.projectExplorer->isClosed());
    EXPECT_TRUE(fixture.information->isClosed());
}

TEST(LayoutZoneToggleTest, RenamedDocksKeepCollapsedGroupsAcrossLayoutStores)
{
    const QList<QPair<QByteArray, QByteArray>> renames = {
        {"AuditionDock", "ControlDock"},
        {"PreviewDock", "ControlDock"},
        {"BlueprintCanvasDock", "BlueprintDock"},
        {"AIDock", "AssistantDock"},
        {"MonitorCatalogDock", "MonitorDock"},
        {"PackManagerDock", "PublisherDock"},
    };
    for (const auto& [oldName, currentName] : renames) {
        for (bool namedLayout : {false, true}) {
            SCOPED_TRACE(oldName.toStdString());
            SCOPED_TRACE(namedLayout);
            ZoneFixture fixture;
            NamedLayoutGuard guard{QStringLiteral("RenamedZoneTestLayout")};
            auto* panel = fixture.manager->findDockWidget(QString::fromLatin1(currentName));
            if (!panel) {
                panel = makeDock(QString::fromLatin1(currentName));
                fixture.manager->addDockWidget(ads::BottomDockWidgetArea, panel);
            }
            auto* companion = makeDock(QStringLiteral("CompanionDock"));
            auto* closed = makeDock(QStringLiteral("ClosedDock"));
            fixture.manager->addDockWidget(ads::CenterDockWidgetArea, companion,
                                            panel->dockAreaWidget());
            fixture.manager->addDockWidget(ads::CenterDockWidgetArea, closed,
                                            panel->dockAreaWidget());
            panel->toggleView(true);
            closed->toggleView(false);
            fixture.controller->setZoneCollapsed(panel, true);
            fixture.controller->saveProjectLayout();

            QJsonObject saved = fixture.cachedLayout();
            QByteArray state = QByteArray::fromBase64(saved.value(QStringLiteral("state")).toString().toLatin1());
            if (!state.startsWith("<?xml"))
                state = qUncompress(state);
            state.replace(currentName, oldName);
            saved[QStringLiteral("state")] = QString::fromLatin1(state.toBase64());
            QByteArray collapsed = QJsonDocument(saved.value(QStringLiteral("collapsedZones")).toArray()).toJson();
            collapsed.replace(currentName, oldName);
            saved[QStringLiteral("collapsedZones")] = QJsonDocument::fromJson(collapsed).array();

            fixture.controller->setZoneCollapsed(panel, false);
            if (namedLayout) {
                saved[QStringLiteral("name")] = guard.name;
                saved[QStringLiteral("dockState")] = saved.value(QStringLiteral("state"));
                ASSERT_TRUE(VNX::NamedLayoutStore::write(guard.name, saved));
                ASSERT_TRUE(fixture.controller->loadNamedLayout(guard.name));
            } else {
                QFile file(VNX::PathUtils::getDotCacheDir(fixture.projectDir.path())
                           + QStringLiteral("/editor_layout.json"));
                ASSERT_TRUE(file.open(QIODevice::WriteOnly));
                const QByteArray data = QJsonDocument(saved).toJson();
                ASSERT_EQ(file.write(data), data.size());
                file.close();
                fixture.reopenFromCache();
            }

            EXPECT_TRUE(panel->isClosed());
            EXPECT_TRUE(companion->isClosed());
            EXPECT_TRUE(closed->isClosed());
            fixture.controller->setZoneCollapsed(panel, false);
            QApplication::processEvents();
            EXPECT_FALSE(panel->isClosed());
            EXPECT_FALSE(companion->isClosed());
            EXPECT_TRUE(closed->isClosed());
        }
    }
}

TEST(LayoutZoneToggleTest, ACacheWithoutCollapseRecordsLeavesEveryZoneOpen)
{
    ZoneFixture fixture;
    fixture.controller->saveProjectLayout();

    fixture.reopenFromCache();

    EXPECT_FALSE(fixture.projectExplorer->isClosed());
    EXPECT_FALSE(fixture.information->isClosed());
    EXPECT_FALSE(fixture.ai->isClosed());
}

TEST(LayoutZoneToggleTest, CollapsingRightZoneLeavesTheLeftColumnAlone)
{
    ZoneFixture fixture;

    fixture.controller->setZoneCollapsed(fixture.ai, true);

    EXPECT_TRUE(fixture.ai->isClosed());
    EXPECT_FALSE(fixture.projectExplorer->isClosed());
    EXPECT_FALSE(fixture.information->isClosed());
    EXPECT_FALSE(fixture.canvas->isClosed());
}

TEST(LayoutZoneToggleTest, CentralTabRevealKeepsGridWithBlueprint)
{
    ZoneFixture fixture;
    ads::CDockWidget* grid = makeDock(QStringLiteral("GridEditorDock_1"));
    ads::CDockAreaWidget* canvasArea = fixture.canvas->dockAreaWidget();
    ASSERT_NE(canvasArea, nullptr);

    fixture.manager->addDockWidget(ads::CenterDockWidgetArea, grid, canvasArea);
    grid->toggleView(true);
    fixture.controller->setPlacementHint(
        grid->objectName(), VNX::MainWindow::PreferredOpenArea::CentralTab);
    QApplication::processEvents();
    EXPECT_EQ(grid->dockAreaWidget(), fixture.canvas->dockAreaWidget());

    fixture.controller->revealDock(grid);
    QApplication::processEvents();
    EXPECT_EQ(grid->dockAreaWidget(), fixture.canvas->dockAreaWidget());
}

}
