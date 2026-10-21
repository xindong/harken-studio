#include "WorkspaceWindowTypes.h"
#include "Core/WindowPlacement.h"
#include "WorkspaceWindowManager.h"
#include "PanelInstanceRegistry.h"
#include "Widgets/SourceEditor/SourceEditorPanel.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QTemporaryDir>

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

TEST(EditorWorkspaceWindowContract, WindowFrameIsCenteredInTheSelectedWorkArea)
{
    for (const QRect& available : QList<QRect>{{0, 40, 1920, 1040},
                                               {-1920, 40, 1920, 1040},
                                               {1920, 80, 1536, 824}}) {
        const QRect frame = centeredWindowFrame({1100, 700}, available);
        EXPECT_TRUE(available.contains(frame));
        EXPECT_EQ(frame.size(), QSize(1100, 700));
        EXPECT_LE(qAbs(frame.center().x() - available.center().x()), 1);
        EXPECT_LE(qAbs(frame.center().y() - available.center().y()), 1);
    }
}

TEST(EditorWorkspaceWindowContract, OversizedWindowFramesFitTheSelectedScreen)
{
    const QRect available(-1920, 40, 1312, 734);
    EXPECT_EQ(centeredWindowFrame(kDefaultStudioWindowSize, available), available);
    EXPECT_FALSE(centeredWindowFrame({}, available).isValid());
    EXPECT_FALSE(centeredWindowFrame({1000, 700}, {}).isValid());
}

TEST(EditorWorkspaceWindowContract, PresentationRoundTripStoresSizeAndStateWithoutCoordinates)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath("window.ini"), QSettings::IniFormat);
    for (const Qt::WindowStates state : {Qt::WindowStates{},
             Qt::WindowStates(Qt::WindowMaximized), Qt::WindowStates(Qt::WindowFullScreen),
             Qt::WindowStates(Qt::WindowMinimized | Qt::WindowMaximized)}) {
        QWidget window;
        window.resize(640, 480);
        window.move(-3839, 27);
        window.setWindowState(state);
        settings.setValue("geometry", window.saveGeometry());
        saveWindowPresentation(settings, window);
        const auto presentation = readWindowPresentation(settings);
        EXPECT_EQ(presentation.normalSize, QSize(640, 480));
        EXPECT_EQ(presentation.state, persistentWindowState(state));
        EXPECT_FALSE(settings.contains("geometry"));
        EXPECT_EQ(settings.allKeys(), (QStringList{"normalSize", "presentationState"}));
    }
}

TEST(EditorWorkspaceWindowContract, LegacyPresentationMigrationDiscardsPosition)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath("window.ini"), QSettings::IniFormat);
    QWidget legacy;
    legacy.resize(640, 480);
    legacy.move(-3839, 27);
    settings.setValue("geometry", legacy.saveGeometry());
    const auto migrated = readWindowPresentation(settings);
    EXPECT_EQ(migrated.normalSize, QSize(640, 480));
    EXPECT_EQ(migrated.state, Qt::WindowNoState);
    settings.setValue("geometry", QByteArray("invalid"));
    EXPECT_EQ(readWindowPresentation(settings).normalSize, kDefaultStudioWindowSize);
}

TEST(EditorWorkspaceWindowContract, NewPresentationFieldsDoNotMixWithLegacyState)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath("window.ini"), QSettings::IniFormat);
    QWidget legacy;
    legacy.resize(640, 480);
    legacy.setWindowState(Qt::WindowMaximized);
    settings.setValue("geometry", legacy.saveGeometry());
    settings.setValue("normalSize", QSize(1100, 700));
    EXPECT_EQ(readWindowPresentation(settings).normalSize, QSize(1100, 700));
    EXPECT_EQ(readWindowPresentation(settings).state, Qt::WindowNoState);
    settings.setValue("normalSize", QStringLiteral("invalid-size"));
    settings.setValue("presentationState", int(Qt::WindowMinimized));
    EXPECT_EQ(readWindowPresentation(settings).normalSize, kDefaultStudioWindowSize);
    EXPECT_EQ(readWindowPresentation(settings).state, Qt::WindowNoState);
}
