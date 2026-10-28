#include "Monitor/MonitorCatalogModel.h"

#include "Monitor/MonitorTypes.h"
#include "Common/TableItemDelegate.h"
#include "Core/ThemeManager.h"

#include <QStringList>

#include <cstring>

namespace VNX {
namespace {
using namespace Monitor;

QString unknownToken(std::uint32_t value)
{
    return QStringLiteral("Unknown(%1)").arg(value);
}

QString resultDescription(std::int32_t result)
{
    switch (result) {
    case kOk: return MonitorCatalogModel::tr("OK");
    case kInvalidArg: return MonitorCatalogModel::tr("Invalid arguments");
    case kUnknownId: return MonitorCatalogModel::tr("Unknown object");
    case kContentNotLoaded: return MonitorCatalogModel::tr("Content not loaded");
    case kCapabilityUnavailable: return MonitorCatalogModel::tr("Operation unavailable");
    case kStaleHandle: return MonitorCatalogModel::tr("Stale instance");
    case kBackpressure: return MonitorCatalogModel::tr("Queue capacity reached");
    case kNoVoice: return MonitorCatalogModel::tr("Voice capacity reached");
    case kNoAction: return MonitorCatalogModel::tr("Action capacity reached");
    case kNoVariable: return MonitorCatalogModel::tr("Variable not found");
    case kNoVariableLabel: return MonitorCatalogModel::tr("Variable label not found");
    case kNotInitialized: return MonitorCatalogModel::tr("Audio engine unavailable");
    case kInUse: return MonitorCatalogModel::tr("In use");
    case kVariableWriterForbidden: return MonitorCatalogModel::tr("Variable write forbidden");
    case kVariableScopeMismatch: return MonitorCatalogModel::tr("Variable scope mismatch");
    default: return MonitorCatalogModel::tr("Unknown error");
    }
}

QString operationName(std::uint32_t operation)
{
    switch (operation) {
    case kMonitorOperationPlayPreview: return MonitorCatalogModel::tr("Play");
    case kMonitorOperationPlayRawPreview: return MonitorCatalogModel::tr("Play audio");
    case kMonitorOperationResetPreview: return MonitorCatalogModel::tr("Reset playback");
    case kMonitorOperationStopPreview: return MonitorCatalogModel::tr("Stop playback");
    case kMonitorOperationPostAction:
    case kMonitorOperationPostActionByName:
    case kMonitorOperationPostActionPinned:
    case kMonitorOperationPostActionResolved: return MonitorCatalogModel::tr("Action");
    case kMonitorOperationStopVoice: return MonitorCatalogModel::tr("Stop playback");
    case kMonitorOperationStopAction: return MonitorCatalogModel::tr("Stop Action");
    case kMonitorOperationStopAll: return MonitorCatalogModel::tr("Stop all");
    case kMonitorOperationStopEmitter: return MonitorCatalogModel::tr("Stop emitter");
    default: return MonitorCatalogModel::tr("Operation %1").arg(operation);
    }
}

QString executionDescription(const CapturedEvent& event)
{
    const auto& fact = event.execution;
    const auto operation = monitorFactHas(fact, kMonitorFactFieldRequestOperation)
        ? operationName(fact.request.operation) : MonitorCatalogModel::tr("Request");
    switch (fact.factKind) {
    case kMonitorFactRequestReturned:
        if (monitorFactHas(fact, kMonitorFactFieldResult) && fact.result != kOk)
            return MonitorCatalogModel::tr("%1 failed: %2").arg(operation, resultDescription(fact.result));
        return {};
    case kMonitorFactActionAccepted: return MonitorCatalogModel::tr("Triggered");
    case kMonitorFactActionTerminal:
        switch (event.activity.actionOutcome) {
        case 1: return MonitorCatalogModel::tr("Finished");
        case 2: return MonitorCatalogModel::tr("Finished with errors");
        case 3: return MonitorCatalogModel::tr("Failed");
        case 4: return fact.terminalKind == kMonitorTerminalRequestedStop
            ? MonitorCatalogModel::tr("Stopped") : MonitorCatalogModel::tr("Cancelled");
        default: return {};
        }
    case kMonitorFactStepExecuted: {
        QString step;
        switch (event.activity.stepOpcode) {
        case 0: step = MonitorCatalogModel::tr("Play"); break;
        case 1: step = MonitorCatalogModel::tr("Stop"); break;
        case 2: step = MonitorCatalogModel::tr("Post Action"); break;
        case 3: step = MonitorCatalogModel::tr("Set Variable"); break;
        case 4: step = MonitorCatalogModel::tr("Reset"); break;
        default: return {};
        }
        return MonitorCatalogModel::tr("Step %1: %2").arg(event.activity.stepIndex + 1).arg(step);
    }
    case kMonitorFactVariableResult:
        switch (event.activity.variableOutcome) {
        case 0: return MonitorCatalogModel::tr("Setting failed: %1").arg(resultDescription(fact.result));
        case 1: {
            float value;
            std::memcpy(&value, &event.activity.variableValueBits, sizeof(value));
            return MonitorCatalogModel::tr("Set to %1").arg(value, 0, 'g', 7);
        }
        case 2: return MonitorCatalogModel::tr("Not applied: replaced by another value");
        case 3: return MonitorCatalogModel::tr("Not applied: content changed");
        case 4: return MonitorCatalogModel::tr("Not applied: playback reset");
        case 5: return MonitorCatalogModel::tr("Not applied: game object unregistered");
        case 6: return MonitorCatalogModel::tr("Not applied: session ended");
        case 7: return MonitorCatalogModel::tr("Not applied: target unavailable");
        default: return {};
        }
    case kMonitorFactEmitterCreated: return MonitorCatalogModel::tr("Registered");
    case kMonitorFactEmitterDestroyed: return MonitorCatalogModel::tr("Unregistered");
    case kMonitorFactActionRejected: return MonitorCatalogModel::tr("Action rejected");
    case kMonitorFactTransitionStarted: return MonitorCatalogModel::tr("Transition started");
    case kMonitorFactTransitionStopped: return MonitorCatalogModel::tr("Transition stopped");
    case kMonitorFactTransitionRejected: return MonitorCatalogModel::tr("Transition rejected");
    case kMonitorFactSourceApplied: return MonitorCatalogModel::tr("Playback source changed");
    case kMonitorFactSeekApplied: return MonitorCatalogModel::tr("Playback position changed");
    default: return {};
    }
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
    connect(capture, &Monitor::EventCaptureController::recordsUpdated, this,
        [this](int first, int last) { emit dataChanged(index(first, 0), index(last, ColumnCount - 1)); });
    connect(capture, &Monitor::EventCaptureController::projectObjectsChanged, this, [this] {
        if (rowCount() > 0)
            emit dataChanged(index(0, Object), index(rowCount() - 1, Object),
                             {ProjectObjectIdRole, TableCursorRole, Qt::DecorationRole});
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        objectIcons_.clear();
        if (rowCount() > 0)
            emit dataChanged(index(0, Object), index(rowCount() - 1, Object), {Qt::DecorationRole});
    });
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
    case Time: return tr("Time");
    case Level: return tr("Level");
    case Type: return tr("Type");
    case Description: return tr("Description");
    case RequestId: return tr("Request ID");
    case Object: return tr("Object");
    case ObjectId: return tr("Object ID");
    case GameObject: return tr("Game Object");
    case GameObjectId: return tr("Game Object ID");
    default: return {};
    }
}

QVariant MonitorCatalogModel::data(const QModelIndex& index, int role) const
{
    const auto* event = index.isValid() ? eventAt(index.row()) : nullptr;
    if (!event || !event->isDesignerEvent()) return {};
    if (index.column() == Object) {
        if (role == ProjectObjectIdRole) return capture_->matchingProjectObject(*event);
        if (role == TableCursorRole)
            return static_cast<int>(capture_->matchingProjectObject(*event) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (role == Qt::DecorationRole) {
            const auto* object = capture_->matchingProjectObjectLocation(*event);
            if (!object || !object->ownerObjectId) return {};
            auto icon = objectIcons_.constFind(object->ownerIconId);
            if (icon == objectIcons_.cend())
                icon = objectIcons_.insert(object->ownerIconId, ThemeManager::instance().resolveIcon(object->ownerIconId).icon);
            return *icon;
        }
    }
    if (role == Qt::ToolTipRole) return details(index.row());
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case Time: {
        if (event->isExecution() && !monitorFactHas(event->execution, kMonitorFactFieldTimestamp)) return {};
        const auto timestamp = event->timestampNs();
        const auto start = capture_->startedTimestampNs();
        const double elapsed = timestamp >= start ? static_cast<double>(timestamp - start) : -static_cast<double>(start - timestamp);
        return QString::number(elapsed / 1e9, 'f', 3);
    }
    case Level: return levelText(*event);
    case Type: return typeText(*event);
    case Description: return description(*event);
    case RequestId:
        if (event->playbackRequestId()) return QString::number(event->playbackRequestId());
        return {};
    case Object: return event->objectName;
    case ObjectId:
        if (!harkenstudio::IsNull(event->objectId())) return objectIdString(event->objectId());
        return {};
    case GameObject: return event->gameObjectName;
    case GameObjectId:
        if (event->gameObjectId()) return QString::number(event->gameObjectId());
        return {};
    default: return {};
    }
}

QString MonitorCatalogModel::levelText(const Monitor::CapturedEvent& event)
{
    switch (event.level()) {
    case VNX::Monitor::kLogInfo: return tr("Info");
    case VNX::Monitor::kLogWarn: return tr("Warning");
    case VNX::Monitor::kLogError: return tr("Error");
    default: return unknownToken(event.level());
    }
}

QString MonitorCatalogModel::typeText(const Monitor::CapturedEvent& event)
{
    switch (event.type()) {
    case DesignerEventType::Action: return tr("Action");
    case DesignerEventType::Step: return tr("Step");
    case DesignerEventType::Playable: return tr("Playable");
    case DesignerEventType::Variable: return tr("Variable");
    case DesignerEventType::GameObject: return tr("Game Object");
    default: return {};
    }
}

QString MonitorCatalogModel::description(const Monitor::CapturedEvent& event)
{
    if (!event.playbackDescription.isEmpty()) return event.playbackDescription;
    if (event.isExecution()) return executionDescription(event);
    const auto& detail = event.detail;
    const QString text = QString::fromUtf8(detail.text, detail.textBytes);
    if (detail.category == VNX::Monitor::kMonitorEventCategoryDiagnostic)
        return tr("%1 [code %2]").arg(text).arg(detail.code);
    if (detail.category == VNX::Monitor::kMonitorEventCategoryLifecycle) {
        QString phase;
        switch (detail.code) {
        case VNX::Monitor::kMonitorLifecycleActionAccept: phase = tr("Action triggered"); break;
        case VNX::Monitor::kMonitorLifecycleActionStop: phase = tr("Action stopping"); break;
        case VNX::Monitor::kMonitorLifecycleActionReject: phase = tr("Action rejected"); break;
        case VNX::Monitor::kMonitorLifecycleTransitionStart: phase = tr("Transition started"); break;
        case VNX::Monitor::kMonitorLifecycleTransitionStop: phase = tr("Transition stopped"); break;
        case VNX::Monitor::kMonitorLifecycleTransitionReject: phase = tr("Transition rejected"); break;
        default: return {};
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
    case VNX::Monitor::kMonitorOperationStopVoice: operation = tr("Stop playback"); break;
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
    return tr("%1: %2 (%3)").arg(operation, resultDescription(detail.result)).arg(detail.result);
}

QString MonitorCatalogModel::details(int row) const
{
    const auto* event = eventAt(row);
    if (!event || !event->isDesignerEvent()) return {};
    QStringList lines;
    for (int column = 0; column < ColumnCount; ++column) {
        const auto value = data(index(row, column)).toString();
        if (!value.isEmpty())
            lines << tr("%1: %2").arg(headerData(column, Qt::Horizontal).toString(), value);
    }
    if (event->isExecution() && monitorFactHas(event->execution, kMonitorFactFieldResult) &&
        event->execution.result != kOk)
        lines << tr("Error code: %1").arg(event->execution.result);
    if (event->type() == DesignerEventType::Step) {
        if (!event->actionName.isEmpty()) lines << tr("Action: %1").arg(event->actionName);
        if (!harkenstudio::IsNull(event->header.stableId))
            lines << tr("Action ID: %1").arg(objectIdString(event->header.stableId));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace VNX
