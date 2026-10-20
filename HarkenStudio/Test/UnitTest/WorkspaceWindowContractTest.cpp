#include "WorkspaceWindowTypes.h"
#include "Core/WindowPlacement.h"
#include "WorkspaceWindowManager.h"
#include "PanelInstanceRegistry.h"
#include "Widgets/SourceEditor/SourceEditorPanel.h"

#include <gtest/gtest.h>

#include <QSet>

using namespace VNX;

namespace {

const WorkspaceWindowDescriptor& requireDescriptor(const WorkspaceWindowCatalog& catalog,
                                                   const QString& typeId)
{
    const WorkspaceWindowDescriptor* descriptor = catalog.find(typeId);
    EXPECT_NE(descriptor, nullptr) << typeId.toStdString();
    return *descriptor;
}

} // namespace

TEST(EditorWorkspaceWindowContract, DefaultDescriptorsHaveStableUniqueIds)
{
    const WorkspaceWindowCatalog catalog = defaultWorkspaceWindowCatalog();
    QSet<QString> typeIds;
    QSet<QString> dockObjectNames;

    for (const WorkspaceWindowDescriptor& descriptor : catalog.descriptors()) {
        EXPECT_FALSE(descriptor.typeId.isEmpty());
        EXPECT_FALSE(descriptor.title.isEmpty());
        EXPECT_FALSE(descriptor.dockObjectName.isEmpty());
        EXPECT_FALSE(typeIds.contains(descriptor.typeId));
        EXPECT_FALSE(dockObjectNames.contains(descriptor.dockObjectName));
        typeIds.insert(descriptor.typeId);
        dockObjectNames.insert(descriptor.dockObjectName);
    }

}

TEST(EditorWorkspaceWindowContract, DisabledWindowsCannotBeCreatedOrRestored)
{
    WorkspaceWindowCatalog catalog;
    WorkspaceWindowDescriptor descriptor;
    descriptor.typeId = QStringLiteral("DisabledWindow");
    descriptor.kind = WindowKind::MultiInstance;
    descriptor.availability = AvailabilityPolicy::FeatureDisabled;
    catalog.registerWindow(descriptor);
    WorkspaceWindowManager manager(std::move(catalog), QtAdsWindowAdapter());

    int creations = 0;
    int restorations = 0;
    WorkspaceWindowManager::MultiInstanceFactory factory;
    factory.create = [&]() { ++creations; return true; };
    factory.recreate = [&](const WorkspaceWindowInstanceSpec&) { ++restorations; return true; };
    manager.registerMultiInstanceFactory(descriptor.typeId, std::move(factory));

    WorkspaceWindowInstanceSpec saved;
    saved.typeId = descriptor.typeId;
    saved.instanceId = 1;
    EXPECT_FALSE(manager.createInstance(descriptor.typeId));
    EXPECT_FALSE(manager.restoreInstance(saved));
    EXPECT_EQ(creations, 0);
    EXPECT_EQ(restorations, 0);
    EXPECT_TRUE(manager.registry().instanceEntries(descriptor.typeId).isEmpty());
}

TEST(EditorWorkspaceWindowContract, MultiInstanceWindowsUseDescriptorRestoreSpecIdentity)
{
    WorkspaceWindowCatalog catalog = defaultWorkspaceWindowCatalog();
    WorkspaceWindowDescriptor inspector;
    inspector.typeId = QStringLiteral("Inspector");
    inspector.title = QStringLiteral("Inspector");
    inspector.dockObjectName = QStringLiteral("InspectorDock");
    inspector.kind = WindowKind::MultiInstance;
    inspector.placement = {DefaultDockArea::Floating, QString(), false};
    inspector.materialization = MaterializationPolicy::OnDemand;
    inspector.closePolicy = ClosePolicy::Destroy;
    catalog.registerWindow(inspector);

    const auto& restoredInspector = requireDescriptor(catalog, QStringLiteral("Inspector"));
    EXPECT_EQ(restoredInspector.kind, WindowKind::MultiInstance);
    EXPECT_EQ(restoredInspector.dockObjectName, QStringLiteral("InspectorDock"));

    WorkspaceWindowInstanceSpec spec;
    spec.typeId = QStringLiteral("Inspector");
    spec.instanceId = 42;
    spec.params.insert(QStringLiteral("bindingGuid"), QStringLiteral("abc"));
    EXPECT_EQ(spec.typeId, restoredInspector.typeId);
    EXPECT_EQ(spec.instanceId, 42);
    EXPECT_EQ(spec.params.value(QStringLiteral("bindingGuid")).toString(), QStringLiteral("abc"));
}

TEST(EditorWorkspaceWindowContract, InstanceSpecJsonPreservesRestoreParameters)
{
    WorkspaceWindowInstanceSpec spec;
    spec.typeId = QStringLiteral("Inspector");
    spec.instanceId = 7;
    spec.deleteOnClose = false;
    spec.params.insert(QStringLiteral("bindingGuid"), QStringLiteral("guid-7"));
    spec.params.insert(QStringLiteral("pinned"), true);
    spec.pinnedObjectGuid = QStringLiteral("object-guid");

    const QJsonArray json = workspaceWindowInstanceSpecsToJson({spec});
    ASSERT_EQ(json.size(), 1);

    const QList<WorkspaceWindowInstanceSpec> restored =
        workspaceWindowInstanceSpecsFromJson(json);
    ASSERT_EQ(restored.size(), 1);
    EXPECT_EQ(restored.first().typeId, spec.typeId);
    EXPECT_EQ(restored.first().instanceId, spec.instanceId);
    EXPECT_EQ(restored.first().deleteOnClose, spec.deleteOnClose);
    EXPECT_EQ(restored.first().params.value(QStringLiteral("bindingGuid")).toString(),
              QStringLiteral("guid-7"));
    EXPECT_TRUE(restored.first().params.value(QStringLiteral("pinned")).toBool());
    EXPECT_EQ(restored.first().pinnedObjectGuid, spec.pinnedObjectGuid);
}

TEST(EditorWorkspaceWindowContract, LegacyCurveEditorRestoresAsExternalEditor)
{
    const auto restored = VNX::workspaceWindowInstanceSpecFromJson({
        {QStringLiteral("typeId"), QStringLiteral("CurveEditor")},
        {QStringLiteral("instanceId"), 7},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("dimension"), QStringLiteral("pitch")}}}});
    EXPECT_EQ(restored.typeId, QStringLiteral("ExternalEditor"));
    EXPECT_EQ(restored.instanceId, 7);
    EXPECT_EQ(restored.params.value(QStringLiteral("dimension")).toString(), QStringLiteral("pitch"));
}

TEST(EditorWorkspaceWindowContract, SourceEditorPrefixIsUnambiguousAmongPanelTypes)
{
    PanelInstanceRegistry registry;
    const QList<QPair<QString, QString>> types = {
        {SourceEditorPanel::typeId(), SourceEditorPanel::dockObjectPrefix()},
        {QStringLiteral("Meter"), QStringLiteral("MeterDock")},
        {QStringLiteral("Inspector"), QStringLiteral("InspectorDock")},
        {QStringLiteral("Hierarchy"), QStringLiteral("HierarchyDock")},
        {QStringLiteral("ExternalEditor"), QStringLiteral("CurveEditorDock")},
        {QStringLiteral("GridEditor"), QStringLiteral("GridEditorDock")},
        {QStringLiteral("TimelineEditor"), QStringLiteral("TimelineEditorDock")},
        {QStringLiteral("TransitionEditor"), QStringLiteral("TransitionEditorDock")},
    };

    for (const auto& [type, prefix] : types) {
        PanelInstanceRegistry::PanelTypeInfo info;
        info.label = type;
        info.dockObjectPrefix = prefix;
        registry.registerPanelType(type, std::move(info));
    }

    const auto* sourceEditor = registry.panelTypeInfo(SourceEditorPanel::typeId());
    ASSERT_NE(sourceEditor, nullptr);
    EXPECT_EQ(sourceEditor->label, SourceEditorPanel::typeId());
    EXPECT_EQ(sourceEditor->dockObjectPrefix, SourceEditorPanel::dockObjectPrefix());
    EXPECT_EQ(registry.registeredPanelTypes().size(), types.size());
    EXPECT_TRUE(registry.isMultiInstanceDock(
        SourceEditorPanel::dockObjectPrefix() + QStringLiteral("_2")));

    for (const auto& [type, prefix] : types) {
        const auto* info = registry.panelTypeInfo(type);
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->dockObjectPrefix, prefix);
        if (prefix == SourceEditorPanel::dockObjectPrefix())
            continue;
        EXPECT_FALSE(prefix.startsWith(SourceEditorPanel::dockObjectPrefix()));
        EXPECT_FALSE(SourceEditorPanel::dockObjectPrefix().startsWith(prefix));
    }
}

TEST(EditorWorkspaceWindowContract, UnreachableFramesReturnToARealScreenWorkArea)
{
    const QList<QRect> screens{{0, 0, 1920, 1040}, {-1920, 0, 1920, 1040}};
    const QList<QRect> frames{
        {1919, 100, 1200, 800},
        {6000, 100, 1200, 800},
        {-6000, 100, 1200, 800},
        {300, -790, 1200, 800},
        {300, 1039, 1200, 800}};
    for (const QRect& frame : frames) {
        const auto placed = visibleWindowPlacement(frame, screens, 0);
        EXPECT_TRUE(placed.available.contains(placed.frame));
        EXPECT_EQ(visibleWindowPlacement(placed.frame, screens, 0).frame, placed.frame);
    }
}

TEST(EditorWorkspaceWindowContract, ValidSecondaryAndReachableSpanningPositionsArePreserved)
{
    const QList<QRect> screens{{0, 0, 1920, 1040}, {-1920, 0, 1920, 1040}};
    const QList<QRect> frames{
        {-1800, 80, 1200, 800},
        {200, 80, 1200, 800},
        {-600, 80, 1200, 800}};
    for (const QRect& frame : frames)
        EXPECT_EQ(visibleWindowPlacement(frame, screens, 0).frame, frame);
}

TEST(EditorWorkspaceWindowContract, GapsBetweenScaledScreensAreNotVisibleDesktop)
{
    const QList<QRect> screens{{0, 0, 1280, 680}, {1920, 0, 1536, 824}};
    const auto placed = visibleWindowPlacement({1400, 50, 300, 500}, screens, 0);
    EXPECT_EQ(placed.available, screens[0]);
    EXPECT_TRUE(screens[0].contains(placed.frame));
    const QRect secondary(2100, 80, 1100, 700);
    EXPECT_EQ(visibleWindowPlacement(secondary, screens, 0).frame, secondary);
}

TEST(EditorWorkspaceWindowContract, OversizedFramesFitAvailableAreaAndRemainStable)
{
    const QList<QRect> screens{{0, 40, 1312, 734}};
    const auto placed = visibleWindowPlacement({300, 100, 1800, 1100}, screens, 0);
    EXPECT_EQ(placed.frame, screens[0]);
    EXPECT_EQ(visibleWindowPlacement(placed.frame, screens, 0).frame, placed.frame);
    EXPECT_EQ(boundedWindowPosition(QRect(placed.frame.topLeft(), QSize(1800, 1100)),
                                   screens[0]), screens[0].topLeft());
}

TEST(EditorWorkspaceWindowContract, InvalidScreenEntriesHaveDeterministicFallback)
{
    const QRect frame(6000, 100, 1000, 700);
    EXPECT_EQ(visibleWindowPlacement(frame, {}, -1).frame, frame);
    const QList<QRect> screens{{}, {-1920, 0, 1920, 1040}, {0, 0, 1920, 1040}};
    EXPECT_EQ(visibleWindowPlacement(frame, screens, 0).available, screens[1]);
    EXPECT_EQ(visibleWindowPlacement(frame, screens, 2).available, screens[2]);
}
