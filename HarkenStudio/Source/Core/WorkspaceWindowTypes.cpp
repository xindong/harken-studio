#include "WorkspaceWindowTypes.h"

#include <QStringList>
#include <QWidget>
#include <utility>

namespace VNX {

void WorkspaceWindowCatalog::registerWindow(WorkspaceWindowDescriptor descriptor)
{
    if (descriptor.typeId.isEmpty())
        return;

    for (WorkspaceWindowDescriptor& existing : descriptors_)
    {
        if (existing.typeId == descriptor.typeId)
        {
            existing = std::move(descriptor);
            return;
        }
    }

    descriptors_.append(std::move(descriptor));
}

bool WorkspaceWindowCatalog::contains(const QString& typeId) const
{
    return find(typeId) != nullptr;
}

const WorkspaceWindowDescriptor* WorkspaceWindowCatalog::find(const QString& typeId) const
{
    for (const WorkspaceWindowDescriptor& descriptor : descriptors_)
    {
        if (descriptor.typeId == typeId)
            return &descriptor;
    }
    return nullptr;
}

QVector<WorkspaceWindowDescriptor> WorkspaceWindowCatalog::descriptors() const
{
    return descriptors_;
}

QVector<WorkspaceWindowDescriptor> defaultWorkspaceWindowDescriptors()
{
    return {
        {
            QStringLiteral("Blueprint"),
            QStringLiteral("Blueprint"),
            QStringLiteral("BlueprintDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Top, QString(), true},
            MaterializationPolicy::Eager,
            ClosePolicy::Hide,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("Assistant"),
            QStringLiteral("Assistant"),
            QStringLiteral("AssistantDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::Eager,
            ClosePolicy::Hide,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("Explorer"),
            QStringLiteral("Explorer"),
            QStringLiteral("ExplorerDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Left, QString(), true},
            MaterializationPolicy::Eager,
            ClosePolicy::Hide,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("Information"),
            QStringLiteral("Information"),
            QStringLiteral("InformationDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Bottom, QStringLiteral("ExplorerDock"), true},
            MaterializationPolicy::ShellEagerContentLazy,
            ClosePolicy::Hide,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("Control"),
            QStringLiteral("Control"),
            QStringLiteral("ControlDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Bottom, QString(), true},
            MaterializationPolicy::Eager,
            ClosePolicy::Hide,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("Log"),
            QStringLiteral("Log"),
            QStringLiteral("LogDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            // Setup/audio paths still append to the LogPanel directly during startup.
            MaterializationPolicy::Eager,
            ClosePolicy::Hide,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("Publisher"),
            QStringLiteral("Publisher"),
            QStringLiteral("PublisherDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            // Pack generation still reads panel options and appends build output directly.
            MaterializationPolicy::Eager,
            ClosePolicy::Hide,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("SourceEditor"),
            QStringLiteral("Source Viewer"),
            QStringLiteral("SourceEditorDock"),
            WindowKind::MultiInstance,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Destroy,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("TransitionEditor"),
            QStringLiteral("Transition Editor"),
            QStringLiteral("TransitionEditorDock"),
            WindowKind::MultiInstance,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Destroy,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorPerformance"),
            QStringLiteral("Performance Monitor"),
            QStringLiteral("MonitorPerformanceDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("Monitor"),
            QStringLiteral("Monitor"),
            QStringLiteral("MonitorDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Bottom, QString(), false},
            MaterializationPolicy::ShellEagerContentLazy,
            ClosePolicy::Hide,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("MonitorVoices"),
            QStringLiteral("Voices"),
            QStringLiteral("MonitorVoicesDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorPlayables"),
            QStringLiteral("Playables & Actions"),
            QStringLiteral("MonitorPlayablesDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorEffects"),
            QStringLiteral("Effects"),
            QStringLiteral("MonitorEffectsDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorBuses"),
            QStringLiteral("Buses"),
            QStringLiteral("MonitorBusesDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorDspTimeline"),
            QStringLiteral("DSP Timeline"),
            QStringLiteral("MonitorDspTimelineDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorLineage"),
            QStringLiteral("Lineage"),
            QStringLiteral("MonitorLineageDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorTapSelect"),
            QStringLiteral("Tap Select"),
            QStringLiteral("MonitorTapSelectDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorWaveform"),
            QStringLiteral("Waveform"),
            QStringLiteral("MonitorWaveformDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorScope"),
            QStringLiteral("Scope"),
            QStringLiteral("MonitorScopeDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorSpectrum"),
            QStringLiteral("Spectrum"),
            QStringLiteral("MonitorSpectrumDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("Meter"),
            QStringLiteral("Meter"),
            QStringLiteral("MeterDock"),
            WindowKind::MultiInstance,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Destroy,
            AvailabilityPolicy::Always
        },
        {
            QStringLiteral("MonitorRecording"),
            QStringLiteral("Recording"),
            QStringLiteral("MonitorRecordingDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorSessionCompare"),
            QStringLiteral("Session Compare"),
            QStringLiteral("MonitorSessionCompareDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorStreaming"),
            QStringLiteral("Streaming"),
            QStringLiteral("MonitorStreamingDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorMemory"),
            QStringLiteral("Memory"),
            QStringLiteral("MonitorMemoryDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorGameModulate"),
            QStringLiteral("Game Modulate"),
            QStringLiteral("MonitorGameModulateDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        },
        {
            QStringLiteral("MonitorEmitters"),
            QStringLiteral("Emitters"),
            QStringLiteral("MonitorEmittersDock"),
            WindowKind::Singleton,
            {DefaultDockArea::Floating, QString(), false},
            MaterializationPolicy::OnDemand,
            ClosePolicy::Hide,
            AvailabilityPolicy::FeatureDisabled
        }
    };
}

QStringList ipcKnownPanelObjectNames()
{
    return {
        QStringLiteral("Explorer"),
        QStringLiteral("InspectorPanel"),
        QStringLiteral("BlueprintPanel"),
        QStringLiteral("ControlPanel"),
        QStringLiteral("PublisherPanel"),
        QStringLiteral("LogPanel"),
        QStringLiteral("MeterPanel"),
        QStringLiteral("MonitorPerformanceDock"),
        QStringLiteral("MonitorDock"),
        QStringLiteral("MonitorVoicesDock"),
        QStringLiteral("MonitorPlayablesDock"),
        QStringLiteral("MonitorEffectsDock"),
        QStringLiteral("MonitorBusesDock"),
        QStringLiteral("MonitorDspTimelineDock"),
        QStringLiteral("MonitorLineageDock"),
        QStringLiteral("MonitorTapSelectDock"),
        QStringLiteral("MonitorWaveformDock"),
        QStringLiteral("MonitorScopeDock"),
        QStringLiteral("MonitorSpectrumDock"),
        QStringLiteral("MonitorRecordingDock"),
        QStringLiteral("MonitorSessionCompareDock"),
        QStringLiteral("MonitorStreamingDock"),
        QStringLiteral("MonitorMemoryDock"),
        QStringLiteral("MonitorGameModulateDock"),
        QStringLiteral("MonitorEmittersDock"),
        QStringLiteral("MonitorCapturePlanBar"),
    };
}

QString ipcKnownPanelObjectNameFromWidget(const QWidget* widget)
{
    const QStringList knownPanels = ipcKnownPanelObjectNames();
    for (const QWidget* current = widget; current != nullptr; current = current->parentWidget())
    {
        const QString name = current->objectName();
        if (knownPanels.contains(name))
            return name;
    }
    return {};
}

QString ipcFocusedWidgetKnownPanelName(const QWidget* focused)
{
    return ipcKnownPanelObjectNameFromWidget(focused);
}

QString ipcLastInteractedWidgetKnownPanelName(const QWidget* widget)
{
    return ipcKnownPanelObjectNameFromWidget(widget);
}

WorkspaceWindowCatalog defaultWorkspaceWindowCatalog()
{
    WorkspaceWindowCatalog catalog;
    for (WorkspaceWindowDescriptor descriptor : defaultWorkspaceWindowDescriptors())
        catalog.registerWindow(std::move(descriptor));
    return catalog;
}

bool workspaceWindowSelectorMatches(const QString& selector, const QString& typeId,
                                   const QString& objectName, const QString& title,
                                   int instanceId)
{
    const QString needle = selector.trimmed();
    if (needle.isEmpty())
        return false;
    if (typeId.compare(needle, Qt::CaseInsensitive) == 0)
        return true;
    if (!objectName.isEmpty() && objectName.compare(needle, Qt::CaseInsensitive) == 0)
        return true;
    if (!title.isEmpty() && title.compare(needle, Qt::CaseInsensitive) == 0)
        return true;
    if (instanceId > 0
        && QStringLiteral("%1_%2").arg(typeId).arg(instanceId)
               .compare(needle, Qt::CaseInsensitive) == 0)
        return true;
    return false;
}

QJsonObject workspaceWindowInstanceSpecToJson(const WorkspaceWindowInstanceSpec& spec)
{
    QJsonObject obj;
    obj["typeId"] = spec.typeId;
    obj["instanceId"] = spec.instanceId;
    obj["type"] = spec.typeId;
    obj["id"] = spec.instanceId;
    obj["deleteOnClose"] = spec.deleteOnClose;
    if (!spec.params.isEmpty())
        obj["params"] = QJsonObject::fromVariantMap(spec.params);
    if (!spec.pinnedObjectGuid.isEmpty())
        obj["pinnedObjectGuid"] = spec.pinnedObjectGuid;
    return obj;
}

WorkspaceWindowInstanceSpec workspaceWindowInstanceSpecFromJson(const QJsonObject& obj)
{
    WorkspaceWindowInstanceSpec spec;
    spec.typeId = obj.value(QLatin1String("typeId")).toString(
        obj.value(QLatin1String("type")).toString());
    if (spec.typeId == QLatin1String("CurveEditor"))
        spec.typeId = QStringLiteral("ExternalEditor");
    spec.instanceId = obj.value(QLatin1String("instanceId")).toInt(
        obj.value(QLatin1String("id")).toInt(0));
    spec.deleteOnClose = obj.value(QLatin1String("deleteOnClose")).toBool(true);
    spec.params = obj.value(QLatin1String("params")).toObject().toVariantMap();
    spec.pinnedObjectGuid = obj.value(QLatin1String("pinnedObjectGuid")).toString();
    return spec;
}

QJsonArray workspaceWindowInstanceSpecsToJson(const QList<WorkspaceWindowInstanceSpec>& specs)
{
    QJsonArray arr;
    for (const WorkspaceWindowInstanceSpec& spec : specs)
        arr.append(workspaceWindowInstanceSpecToJson(spec));
    return arr;
}

QList<WorkspaceWindowInstanceSpec> workspaceWindowInstanceSpecsFromJson(const QJsonArray& arr)
{
    QList<WorkspaceWindowInstanceSpec> specs;
    for (const QJsonValue& value : arr) {
        const WorkspaceWindowInstanceSpec spec =
            workspaceWindowInstanceSpecFromJson(value.toObject());
        if (spec.instanceId > 0 && !spec.typeId.isEmpty())
            specs.append(spec);
    }
    return specs;
}

}
