#include "BuiltinLayouts.h"
#include "IPC/IPCServer.h"
#include "WorkspaceWindowManager.h"
#include "WorkspaceWindowTypes.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSet>
#include <QStringList>
#include <QWidget>

using namespace VNX;

namespace {

QSet<QString> toSet(const QStringList& values)
{
    return QSet<QString>(values.begin(), values.end());
}

const WorkspaceWindowDescriptor& requireDescriptor(const WorkspaceWindowCatalog& catalog,
                                                   const QString& typeId)
{
    const WorkspaceWindowDescriptor* descriptor = catalog.find(typeId);
    EXPECT_NE(descriptor, nullptr) << typeId.toStdString();
    return *descriptor;
}

} // namespace

TEST(MonitorPanelModule, KeysCatalogAndIpcShareTheSameMonitorSets)
{
    const QStringList typeIds = monitorPanelTypeIds();
    const QStringList docks = monitorPanelDockObjectNames();
    ASSERT_FALSE(typeIds.isEmpty());
    ASSERT_EQ(typeIds.size(), docks.size());
    EXPECT_EQ(toSet(typeIds).size(), typeIds.size());
    EXPECT_EQ(toSet(docks).size(), docks.size());

    const WorkspaceWindowCatalog catalog = defaultWorkspaceWindowCatalog();
    QSet<QString> catalogDocks;
    for (const QString& typeId : typeIds)
    {
        const auto& descriptor = requireDescriptor(catalog, typeId);
        EXPECT_EQ(descriptor.typeId, typeId);
        ASSERT_FALSE(descriptor.dockObjectName.isEmpty());
        catalogDocks.insert(descriptor.dockObjectName);
    }
    EXPECT_EQ(catalogDocks, toSet(docks));

    const QStringList ipcNames = ipcKnownPanelObjectNames();
    for (const QString& dock : docks)
        EXPECT_TRUE(ipcNames.contains(dock));
}

TEST(MonitorPanelModule, FocusedAndLastInteractedHandlersShareLookup)
{
    const QStringList docks = monitorPanelDockObjectNames();
    ASSERT_FALSE(docks.isEmpty());

    QWidget host;
    host.setObjectName(docks.first());
    QLabel child(&host);
    child.setObjectName(QStringLiteral("inner"));

    EXPECT_EQ(ipcFocusedWidgetKnownPanelName(&child), docks.first());
    EXPECT_EQ(ipcLastInteractedWidgetKnownPanelName(&child), docks.first());
    EXPECT_EQ(ipcFocusedWidgetKnownPanelName(&host), docks.first());
    EXPECT_EQ(ipcLastInteractedWidgetKnownPanelName(&host), docks.first());

    QWidget unknown;
    unknown.setObjectName(QStringLiteral("NotAKnownPanel"));
    EXPECT_TRUE(ipcFocusedWidgetKnownPanelName(&unknown).isEmpty());
    EXPECT_TRUE(ipcLastInteractedWidgetKnownPanelName(&unknown).isEmpty());
    EXPECT_TRUE(ipcFocusedWidgetKnownPanelName(nullptr).isEmpty());
    EXPECT_TRUE(ipcLastInteractedWidgetKnownPanelName(nullptr).isEmpty());
}

TEST(MonitorPanelModule, IpcHandlersDispatchWithoutInventingMonitorPanels)
{
    IPCServer server(nullptr);
    const QStringList docks = monitorPanelDockObjectNames();

    const QJsonObject focused = server.dispatchCommand(
        QStringLiteral("get_focused_widget"), QJsonObject{});
    EXPECT_EQ(focused.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(focused.value(QStringLiteral("code")).toString(),
              QStringLiteral("NO_MAIN_WINDOW"));
    for (const QString& dock : docks)
        EXPECT_FALSE(QJsonDocument(focused).toJson().contains(dock.toUtf8()));

    const QJsonObject lastInteracted = server.dispatchCommand(
        QStringLiteral("get_last_interacted_widget"), QJsonObject{});
    EXPECT_EQ(lastInteracted.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(lastInteracted.value(QStringLiteral("code")).toString(),
              QStringLiteral("NO_MAIN_WINDOW"));
    for (const QString& dock : docks)
        EXPECT_FALSE(QJsonDocument(lastInteracted).toJson().contains(dock.toUtf8()));
}

TEST(MonitorPanelModule, CatalogDoesNotCreateDocksUntilEnsure)
{
    WorkspaceWindowManager manager(defaultWorkspaceWindowCatalog(), QtAdsWindowAdapter());
    for (const QString& typeId : monitorPanelTypeIds())
    {
        EXPECT_EQ(manager.singletonDock(typeId), nullptr);
        EXPECT_FALSE(manager.isSingletonAvailable(typeId));
        EXPECT_FALSE(manager.registeredSingletonTypeIds().contains(typeId));
        EXPECT_EQ(manager.ensureSingleton(typeId), nullptr);
        EXPECT_EQ(manager.singletonDock(typeId), nullptr);
        EXPECT_FALSE(manager.showSingleton(typeId));
    }
}
