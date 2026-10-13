#include "Monitor/MonitorCatalogModel.h"

#include "Monitor/MonitorTypes.h"

#include <QStringList>

#include <cstring>

namespace VNX {
namespace {

QString unknownToken(std::uint32_t value)
{
    return QStringLiteral("Unknown(%1)").arg(value);
}

} // namespace

MonitorCatalogModel::MonitorCatalogModel(Monitor::EventCaptureController* capture, QObject* parent)
    : QAbstractTableModel(parent), capture_(capture)
{
    connect(capture, &Monitor::EventCaptureController::recordsAboutToReset, this, [this] { beginResetModel(); });
    connect(capture, &Monitor::EventCaptureController::recordsReset, this, [this] { endResetModel(); });
    connect(capture, &Monitor::EventCaptureController::recordsAboutToInsert, this,
        [this](int first, int last) { beginInsertRows({}, first, last); });
    connect(capture, &Monitor::EventCaptureController::recordsInserted, this, [this] { endInsertRows(); });
    connect(capture, &Monitor::EventCaptureController::recordsAboutToRemove, this,
        [this](int first, int last) { beginRemoveRows({}, first, last); });
    connect(capture, &Monitor::EventCaptureController::recordsRemoved, this, [this] { endRemoveRows(); });
    connect(capture, &QObject::destroyed, this, [this] { beginResetModel(); endResetModel(); });
}

int MonitorCatalogModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || !capture_ ? 0 : capture_->rowCount();
}

int MonitorCatalogModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

const Monitor::CapturedEvent* MonitorCatalogModel::eventAt(int row) const
{
    return capture_ && row >= 0 && row < capture_->rowCount() ? &capture_->eventAt(row) : nullptr;
}

QVariant MonitorCatalogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Time: return tr("Time (s)");
    case Level: return tr("Level");
    case Category: return tr("Category");
    case Object: return tr("Object");
    case Description: return tr("Description");
    default: return {};
    }
}

QVariant MonitorCatalogModel::data(const QModelIndex& index, int role) const
{
    const auto* event = index.isValid() ? eventAt(index.row()) : nullptr;
    if (!event) return {};
    if (role == Qt::ToolTipRole) return details(index.row());
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case Time: return QString::number(static_cast<double>(event->detail.timestampNs -
        capture_->startedTimestampNs()) / 1e9, 'f', 6);
    case Level: return levelText(*event);
    case Category: return categoryText(*event);
    case Object:
        if (!event->objectName.isEmpty()) return event->objectName;
        if (!harkenstudio::IsNull(event->header.stableId)) return objectIdString(event->header.stableId);
        if (event->header.instanceHandle) return tr("Instance %1").arg(event->header.instanceHandle);
        return {};
    case Description: return description(*event);
    default: return {};
    }
}

QString MonitorCatalogModel::levelText(const Monitor::CapturedEvent& event)
{
    switch (event.detail.level) {
    case VNX::Monitor::kLogInfo: return tr("Info");
    case VNX::Monitor::kLogWarn: return tr("Warning");
    case VNX::Monitor::kLogError: return tr("Error");
    default: return unknownToken(event.detail.level);
    }
}

QString MonitorCatalogModel::categoryText(const Monitor::CapturedEvent& event)
{
    switch (event.detail.category) {
    case VNX::Monitor::kMonitorEventCategoryOperation: return tr("Operation");
    case VNX::Monitor::kMonitorEventCategoryLifecycle: return tr("Playback");
    case VNX::Monitor::kMonitorEventCategoryDiagnostic: return tr("Diagnostic");
    default: return unknownToken(event.detail.category);
    }
}

QString MonitorCatalogModel::description(const Monitor::CapturedEvent& event)
{
    const auto& detail = event.detail;
    const QString text = QString::fromUtf8(detail.text, detail.textBytes);
    if (detail.category == VNX::Monitor::kMonitorEventCategoryDiagnostic)
        return tr("%1 [code %2]").arg(text).arg(detail.code);
    if (detail.category == VNX::Monitor::kMonitorEventCategoryLifecycle) {
        QString phase;
        switch (detail.code) {
        case VNX::Monitor::kMonitorLifecyclePlayableStart: phase = tr("Playable started"); break;
        case VNX::Monitor::kMonitorLifecyclePlayableLogicalComplete: phase = tr("Playable logically complete"); break;
        case VNX::Monitor::kMonitorLifecyclePlayableAudibleRetiring: phase = tr("Playable finishing audio"); break;
        case VNX::Monitor::kMonitorLifecyclePlayableRetired: phase = tr("Playable retired"); break;
        case VNX::Monitor::kMonitorLifecyclePlayableCancel: phase = tr("Playable cancelled"); break;
        case VNX::Monitor::kMonitorLifecycleActionAccept: phase = tr("Action accepted"); break;
        case VNX::Monitor::kMonitorLifecycleActionStop: phase = tr("Action stopped"); break;
        case VNX::Monitor::kMonitorLifecycleActionResult: phase = tr("Action result"); break;
        case VNX::Monitor::kMonitorLifecycleActionReject: phase = tr("Action rejected"); break;
        case VNX::Monitor::kMonitorLifecycleQueueAdvance: phase = tr("Queue advanced"); break;
        case VNX::Monitor::kMonitorLifecycleQueueFail: phase = tr("Queue failed"); break;
        case VNX::Monitor::kMonitorLifecycleTransitionStart: phase = tr("Transition started"); break;
        case VNX::Monitor::kMonitorLifecycleTransitionStop: phase = tr("Transition stopped"); break;
        case VNX::Monitor::kMonitorLifecycleTransitionReject: phase = tr("Transition rejected"); break;
        case VNX::Monitor::kMonitorLifecycleVoicePending: phase = tr("Voice pending"); break;
        case VNX::Monitor::kMonitorLifecycleVoiceLive: phase = tr("Voice live"); break;
        case VNX::Monitor::kMonitorLifecycleVoicePark: phase = tr("Voice parked"); break;
        case VNX::Monitor::kMonitorLifecycleVoiceEnd: phase = tr("Voice ended"); break;
        case VNX::Monitor::kMonitorLifecycleEffectInstalled: phase = tr("Effect installed"); break;
        case VNX::Monitor::kMonitorLifecycleEffectReleased: phase = tr("Effect released"); break;
        default: phase = tr("Playback phase %1").arg(detail.code); break;
        }
        if (detail.result) phase += tr("; reason %1").arg(detail.result);
        return phase;
    }
    QString operation;
    switch (detail.code) {
    case VNX::Monitor::kMonitorOperationPostAction: operation = tr("Post Action"); break;
    case VNX::Monitor::kMonitorOperationPostActionByName: operation = tr("Post Action by name"); break;
    case VNX::Monitor::kMonitorOperationPostActionPinned: operation = tr("Post pinned Action"); break;
    case VNX::Monitor::kMonitorOperationPostActionResolved: operation = tr("Post resolved Action"); break;
    case VNX::Monitor::kMonitorOperationPlayPreview: operation = tr("Play preview"); break;
    case VNX::Monitor::kMonitorOperationStopPreview: operation = tr("Stop preview"); break;
    case VNX::Monitor::kMonitorOperationStopVoice: operation = tr("Stop voice"); break;
    case VNX::Monitor::kMonitorOperationStopAction: operation = tr("Stop Action"); break;
    case VNX::Monitor::kMonitorOperationStopAll: operation = tr("Stop all"); break;
    case VNX::Monitor::kMonitorOperationStopEmitter: operation = tr("Stop emitter"); break;
    case VNX::Monitor::kMonitorOperationSetValue: operation = tr("Set variable"); break;
    case VNX::Monitor::kMonitorOperationSetEmitterValue: operation = tr("Set emitter variable"); break;
    case VNX::Monitor::kMonitorOperationSetValues: operation = tr("Set variable batch"); break;
    case VNX::Monitor::kMonitorOperationSetEmitterValues: operation = tr("Set emitter variable batch"); break;
    case VNX::Monitor::kMonitorOperationLoadPack: operation = tr("Load Pack"); break;
    case VNX::Monitor::kMonitorOperationUnloadPack: operation = tr("Unload Pack"); break;
    default: operation = tr("Operation %1").arg(detail.code); break;
    }
    if (!text.isEmpty()) operation += QStringLiteral(" \"%1\"").arg(text);
    if (detail.code == VNX::Monitor::kMonitorOperationSetValue || detail.code == VNX::Monitor::kMonitorOperationSetEmitterValue ||
        detail.code == VNX::Monitor::kMonitorOperationSetValues || detail.code == VNX::Monitor::kMonitorOperationSetEmitterValues) {
        float value;
        std::memcpy(&value, &detail.valueBits, sizeof(value));
        operation += tr(" = %1").arg(value, 0, 'g', 7);
        if (detail.code == VNX::Monitor::kMonitorOperationSetValues || detail.code == VNX::Monitor::kMonitorOperationSetEmitterValues)
            operation += tr(" [%1/%2]").arg(detail.arg1 ? detail.arg0 + 1 : 0).arg(detail.arg1);
    }
    if (detail.code == VNX::Monitor::kMonitorOperationStopPreview || detail.code == VNX::Monitor::kMonitorOperationStopVoice ||
        detail.code == VNX::Monitor::kMonitorOperationStopAction || detail.code == VNX::Monitor::kMonitorOperationStopAll ||
        detail.code == VNX::Monitor::kMonitorOperationStopEmitter)
        operation += tr("; fade %1 ms").arg(detail.arg0);
    QString result;
    switch (detail.result) {
    case VNX::Monitor::kOk: result = tr("OK"); break;
    case VNX::Monitor::kInvalidArg: result = tr("Invalid arguments"); break;
    case VNX::Monitor::kUnknownId: result = tr("Unknown object"); break;
    case VNX::Monitor::kContentNotLoaded: result = tr("Content not loaded"); break;
    case VNX::Monitor::kStaleHandle: result = tr("Stale instance"); break;
    case VNX::Monitor::kBackpressure: result = tr("Queue capacity reached"); break;
    case VNX::Monitor::kNoVoice: result = tr("Voice capacity reached"); break;
    case VNX::Monitor::kNoAction: result = tr("Action capacity reached"); break;
    case VNX::Monitor::kInUse: result = tr("In use"); break;
    case VNX::Monitor::kVariableWriterForbidden: result = tr("Variable write forbidden"); break;
    case VNX::Monitor::kVariableScopeMismatch: result = tr("Variable scope mismatch"); break;
    default: result = tr("Failed"); break;
    }
    return tr("%1: %2 (%3)").arg(operation, result).arg(detail.result);
}

QString MonitorCatalogModel::details(int row) const
{
    const auto* event = eventAt(row);
    if (!event) return {};
    const auto& h = event->header;
    const auto& d = event->detail;
    return QStringList{
        description(*event),
        tr("Object: %1").arg(event->objectName.isEmpty() ? tr("Unresolved") : event->objectName),
        tr("Target: %1").arg(capture_->targetText()),
        tr("Level: %1 | Category: %2 | Code: %3 | Result / reason: %4")
            .arg(levelText(*event), categoryText(*event)).arg(d.code).arg(d.result),
        tr("Timestamp: %1 ns | Execute quantum: %2").arg(d.timestampNs).arg(h.executeFrame),
        tr("Engine: %1 | Session: %2 | World: %3").arg(h.engineMonitorInstanceId)
            .arg(h.monitorSessionEpoch).arg(h.worldGeneration),
        tr("Object ID: %1 | Instance: %2 | Generation: %3").arg(objectIdString(h.stableId))
            .arg(h.instanceHandle).arg(h.instanceGeneration),
        tr("Project: %1 | Object GUID: %2").arg(event->projectGuid.toString(QUuid::WithoutBraces),
                                               event->objectGuid.toString(QUuid::WithoutBraces)),
        tr("Action instance: %1 | Playable instance: %2").arg(h.actionInstanceHandle).arg(h.playableInstanceHandle),
        tr("Producer: %1 | Sequence: %2 | Fingerprint: %3").arg(h.producerId).arg(h.sourceSequence)
            .arg(h.programSemanticFingerprint, 0, 16),
        tr("Argument 0: %1 | Argument 1: %2 | Value bits: %3").arg(d.arg0).arg(d.arg1).arg(d.valueBits, 0, 16)
    }.join(QLatin1Char('\n'));
}

} // namespace VNX
