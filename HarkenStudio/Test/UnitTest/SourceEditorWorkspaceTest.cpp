#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Core/AppSettings.h"
#include "Core/LayoutController.h"
#include "Core/PanelInstanceRegistry.h"
#include "Core/WorkspaceDockFactory.h"
#include "Core/WorkspaceWindowManager.h"
#include "Widgets/SourceEditor/SourceEditorPanel.h"

#include <gtest/gtest.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QTemporaryDir>

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

class SourceEditorWorkspaceFixture
{
public:
    SourceEditorWorkspaceFixture()
    {
        ensureApp();
        EXPECT_TRUE(projectDir.isValid());
        VNX::AssetRegistry::instance().setProjectRoot(projectDir.path());

        window = std::make_unique<QMainWindow>();
        window->resize(1200, 800);
        dockManager = new ads::CDockManager(window.get());

        baseDock = new ads::CDockWidget(QStringLiteral("Blueprint Canvas"));
        baseDock->setObjectName(QStringLiteral("BlueprintCanvasDock"));
        baseDock->setWidget(new QLabel(QStringLiteral("canvas")));
        dockManager->addDockWidget(ads::TopDockWidgetArea, baseDock);

        VNX::WorkspaceWindowCatalog catalog = VNX::defaultWorkspaceWindowCatalog();
        VNX::WorkspaceWindowDescriptor descriptor;
        descriptor.typeId = VNX::SourceEditorPanel::typeId();
        descriptor.title = QStringLiteral("Source Editor");
        descriptor.dockObjectName = VNX::SourceEditorPanel::dockObjectPrefix();
        descriptor.kind = VNX::WindowKind::MultiInstance;
        descriptor.placement = {VNX::DefaultDockArea::Floating, QString(), false};
        descriptor.materialization = VNX::MaterializationPolicy::OnDemand;
        descriptor.closePolicy = VNX::ClosePolicy::Destroy;
        catalog.registerWindow(std::move(descriptor));

        workspace = std::make_unique<VNX::WorkspaceWindowManager>(
            std::move(catalog), VNX::QtAdsWindowAdapter(dockManager, {}));

        VNX::PanelInstanceRegistry::PanelTypeInfo panelType;
        panelType.label = QStringLiteral("Source Editor");
        panelType.dockObjectPrefix = VNX::SourceEditorPanel::dockObjectPrefix();
        panels.registerPanelType(VNX::SourceEditorPanel::typeId(), std::move(panelType));

        workspace->registerMultiInstanceFactory(VNX::SourceEditorPanel::typeId(), {
            [this]() {
                return createPanel(panels.allocatePanelId(VNX::SourceEditorPanel::typeId()), false)
                    != nullptr;
            },
            [this](const VNX::WorkspaceWindowInstanceSpec& spec) {
                ++recreateCount;
                VNX::SourceEditorPanel* panel = qobject_cast<VNX::SourceEditorPanel*>(
                    panels.findPanel(spec.typeId, spec.instanceId));
                if (!panel)
                    panel = createPanel(spec.instanceId, true);
                if (!panel)
                    return false;
                return applyRestore(spec);
            },
            [this](const VNX::WorkspaceWindowInstanceSpec& spec) {
                return applyRestore(spec);
            },
            [](int id, QWidget* content, ads::CDockWidget* dock) {
                VNX::WorkspaceWindowInstanceSpec spec;
                spec.typeId = VNX::SourceEditorPanel::typeId();
                spec.instanceId = id;
                spec.deleteOnClose = dock && dock->features().testFlag(
                    ads::CDockWidget::DockWidgetDeleteOnClose);
                if (auto* panel = qobject_cast<VNX::SourceEditorPanel*>(content)) {
                    spec.params = panel->snapshotState();
                    spec.params.insert(QStringLiteral("pinned"), panel->isPinned());
                }
                return spec;
            }
        });

        controller = std::make_unique<VNX::LayoutController>(nullptr);
        VNX::LayoutController::Docks docks;
        docks.dockManager = dockManager;
        docks.blueprintCanvasDock = baseDock;
        controller->setDocks(docks);
        controller->setInstanceSnapshotProvider([this] {
            return workspace->instanceSpecs();
        });
        controller->setInstanceRestoreFactory([this](const VNX::DockInstanceSpec& spec) {
            return workspace->restoreInstance(spec);
        });

        window->show();
        QApplication::processEvents();
    }

    ~SourceEditorWorkspaceFixture()
    {
        controller.reset();
        window.reset();
        workspace.reset();
        VNX::AssetRegistry::instance().clear();
    }

    VNX::SourceEditorPanel* createPanel(int id, bool deferShow)
    {
        auto* panel = new VNX::SourceEditorPanel;
        panel->setPanelId(id);
        auto* dock = VNX::WorkspaceDockFactory::createDock(dockManager, {
            QStringLiteral("Source Editor"),
            VNX::SourceEditorPanel::dockObjectPrefix() + QStringLiteral("_%1").arg(id),
            panel,
            true,
            !deferShow
        });
        if (!dock) {
            delete panel;
            return nullptr;
        }

        panels.registerPanel(VNX::SourceEditorPanel::typeId(), id, panel);
        workspace->registerInstance(VNX::SourceEditorPanel::typeId(), id, dock, panel);
        QObject::connect(dock, &QObject::destroyed, &panels, [this, id] {
            panels.unregisterPanel(VNX::SourceEditorPanel::typeId(), id);
            workspace->unregisterInstance(VNX::SourceEditorPanel::typeId(), id);
        });
        return panel;
    }

    bool applyRestore(const VNX::WorkspaceWindowInstanceSpec& spec)
    {
        auto* panel = qobject_cast<VNX::SourceEditorPanel*>(
            panels.findPanel(spec.typeId, spec.instanceId));
        const bool registered = panel
            && workspace->registry().containsInstance(spec.typeId, spec.instanceId);
        restoreAppliedAfterRegistration = restoreAppliedAfterRegistration && registered;
        if (!registered)
            return false;
        panel->restoreState(spec.params);
        panel->setPinned(spec.params.value(QStringLiteral("pinned")).toBool());
        return true;
    }

    void closeSourceEditors()
    {
        const QList<VNX::WorkspaceWindowInstanceEntry> entries =
            workspace->registry().instanceEntries(VNX::SourceEditorPanel::typeId());
        for (const VNX::WorkspaceWindowInstanceEntry& entry : entries)
            entry.dock->closeDockWidget();
        qApp->sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }

    QString layoutPath() const
    {
        return VNX::PathUtils::getDotCacheDir(projectDir.path())
            + QStringLiteral("/editor_layout.json");
    }

    QTemporaryDir projectDir;
    std::unique_ptr<QMainWindow> window;
    ads::CDockManager* dockManager = nullptr;
    ads::CDockWidget* baseDock = nullptr;
    VNX::PanelInstanceRegistry panels;
    std::unique_ptr<VNX::WorkspaceWindowManager> workspace;
    std::unique_ptr<VNX::LayoutController> controller;
    bool restoreAppliedAfterRegistration = true;
    int recreateCount = 0;
};

TEST(SourceEditorWorkspaceTest, TwoInstancesRoundTripThroughProjectLayout)
{
    SourceEditorWorkspaceFixture fixture;
    ASSERT_TRUE(fixture.workspace->createInstance(VNX::SourceEditorPanel::typeId()));
    ASSERT_TRUE(fixture.workspace->createInstance(VNX::SourceEditorPanel::typeId()));

    auto* first = qobject_cast<VNX::SourceEditorPanel*>(
        fixture.workspace->instanceContent(VNX::SourceEditorPanel::typeId(), 1));
    auto* second = qobject_cast<VNX::SourceEditorPanel*>(
        fixture.workspace->instanceContent(VNX::SourceEditorPanel::typeId(), 2));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const QUuid firstSound = QUuid::createUuid();
    const QUuid firstEntry = QUuid::createUuid();
    const QUuid secondSound = QUuid::createUuid();
    const QUuid secondEntry = QUuid::createUuid();
    first->bindSource(101, firstSound, firstEntry);
    first->setPinned(true);
    second->bindSource(202, secondSound, secondEntry);

    ads::CDockWidget* firstDock = fixture.workspace->instanceDock(
        VNX::SourceEditorPanel::typeId(), 1);
    ads::CDockWidget* secondDock = fixture.workspace->instanceDock(
        VNX::SourceEditorPanel::typeId(), 2);
    fixture.dockManager->addDockWidget(ads::LeftDockWidgetArea, firstDock);
    fixture.dockManager->addDockWidget(ads::RightDockWidgetArea, secondDock);
    QApplication::processEvents();

    fixture.controller->saveProjectLayout();
    fixture.closeSourceEditors();
    ASSERT_TRUE(fixture.workspace->registry().instanceEntries(
        VNX::SourceEditorPanel::typeId()).isEmpty());

    fixture.controller->loadProjectLayout();
    QApplication::processEvents();

    EXPECT_EQ(fixture.recreateCount, 2);
    EXPECT_TRUE(fixture.restoreAppliedAfterRegistration);
    const auto restoredEntries = fixture.workspace->registry().instanceEntries(
        VNX::SourceEditorPanel::typeId());
    ASSERT_EQ(restoredEntries.size(), 2);

    first = qobject_cast<VNX::SourceEditorPanel*>(
        fixture.workspace->instanceContent(VNX::SourceEditorPanel::typeId(), 1));
    second = qobject_cast<VNX::SourceEditorPanel*>(
        fixture.workspace->instanceContent(VNX::SourceEditorPanel::typeId(), 2));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->sourceId(), 101u);
    EXPECT_EQ(first->soundGuid(), firstSound);
    EXPECT_EQ(first->entryId(), firstEntry);
    EXPECT_TRUE(first->isPinned());
    EXPECT_EQ(second->sourceId(), 202u);
    EXPECT_EQ(second->soundGuid(), secondSound);
    EXPECT_EQ(second->entryId(), secondEntry);
    EXPECT_FALSE(second->isPinned());

    firstDock = fixture.workspace->instanceDock(VNX::SourceEditorPanel::typeId(), 1);
    secondDock = fixture.workspace->instanceDock(VNX::SourceEditorPanel::typeId(), 2);
    ASSERT_NE(firstDock, nullptr);
    ASSERT_NE(secondDock, nullptr);
    EXPECT_EQ(firstDock->objectName(), QStringLiteral("SourceEditorDock_1"));
    EXPECT_EQ(secondDock->objectName(), QStringLiteral("SourceEditorDock_2"));
    EXPECT_FALSE(firstDock->isClosed());
    EXPECT_FALSE(secondDock->isClosed());
    EXPECT_FALSE(firstDock->isInFloatingContainer());
    EXPECT_FALSE(secondDock->isInFloatingContainer());
}

TEST(SourceEditorWorkspaceTest, CachedLayoutWithoutSourceEditorsStillRestores)
{
    SourceEditorWorkspaceFixture fixture;

    QJsonObject layout;
    layout.insert(QStringLiteral("state"),
                  QString::fromLatin1(fixture.dockManager->saveState().toBase64()));
    layout.insert(QStringLiteral("instances"), QJsonArray{});

    QFile file(fixture.layoutPath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write(QJsonDocument(layout).toJson()), 0);
    file.close();

    fixture.controller->loadProjectLayout();
    QApplication::processEvents();

    EXPECT_EQ(fixture.recreateCount, 0);
    EXPECT_TRUE(fixture.workspace->registry().instanceEntries(
        VNX::SourceEditorPanel::typeId()).isEmpty());
    EXPECT_FALSE(fixture.baseDock->isClosed());
}

}
