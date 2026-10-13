#include "BusTopologySync.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace HarkenEditor {
namespace Bridge {
namespace Detail {

namespace {

bool SameFloat(float left, float right)
{
    return std::fabs(left - right) <=
           std::numeric_limits<float>::epsilon() * 8.0f;
}

} // namespace

BusTopologySync::BusTopologySync(BusTopologyBackend backend,
                                 harkenstudio::ObjectId masterBusId)
    : m_backend(std::move(backend)),
      m_masterBusId(masterBusId)
{
    RuntimeBusDefinition master;
    master.id = masterBusId;
    master.parentId = {};
    master.name = QStringLiteral("Master");
    m_confirmedBuses.emplace(masterBusId, master);
}

void BusTopologySync::Reset()
{
    m_state = BusTopologyState::Idle;
    m_failure = {};
    m_desiredBuses.clear();
    m_confirmedBuses.clear();
    m_desiredSends.clear();
    m_confirmedSends.clear();
    m_operations.clear();
    m_nextOperation = 0;
    m_propertyReplayOperations.clear();
    m_nextPropertyReplayOperation = 0;
    m_confirmedRevision = 0;
    m_pendingRevision = 0;

    RuntimeBusDefinition master;
    master.id = m_masterBusId;
    master.name = QStringLiteral("Master");
    m_confirmedBuses.emplace(m_masterBusId, master);
}

void BusTopologySync::ConfirmWithoutApply(
    const std::vector<RuntimeBusDefinition>& buses,
    const std::vector<RuntimeBusSendDefinition>& sends)
{
    m_failure = {};
    m_operations.clear();
    m_nextOperation = 0;
    m_propertyReplayOperations.clear();
    m_nextPropertyReplayOperation = 0;
    if (!BuildDesired(buses, sends))
        return;
    m_confirmedBuses = m_desiredBuses;
    m_confirmedSends = m_desiredSends;
    m_confirmedRevision = m_pendingRevision;
    m_state = BusTopologyState::Ready;
}

void BusTopologySync::Submit(
    const std::vector<RuntimeBusDefinition>& buses,
    const std::vector<RuntimeBusSendDefinition>& sends,
    uint32_t propertyFadeMs)
{
    m_failure = {};
    m_operations.clear();
    m_nextOperation = 0;
    if (!BuildDesired(buses, sends) ||
        !BuildOperations(propertyFadeMs))
        return;
    if (m_operations.empty()) {
        for (const auto& entry : m_desiredBuses) {
            auto confirmed = m_confirmedBuses.find(entry.first);
            if (confirmed != m_confirmedBuses.end())
                confirmed->second.name = entry.second.name;
        }
        m_propertyReplayOperations.clear();
        m_nextPropertyReplayOperation = 0;
        m_state = BusTopologyState::Ready;
    } else {
        m_state = BusTopologyState::Syncing;
    }
}

bool BusTopologySync::BuildDesired(
    const std::vector<RuntimeBusDefinition>& buses,
    const std::vector<RuntimeBusSendDefinition>& sends)
{
    std::map<harkenstudio::ObjectId, RuntimeBusDefinition> desiredBuses;
    RuntimeBusDefinition master;
    master.id = m_masterBusId;
    master.name = QStringLiteral("Master");
    desiredBuses.emplace(m_masterBusId, master);

    for (RuntimeBusDefinition bus : buses) {
        if (harkenstudio::IsNull(bus.id)) {
            Fail(BusTopologyOperation::ApplySnapshot, kEngineInvalidArg,
                 bus.id, bus.parentId, bus.name);
            return false;
        }
        if (bus.id == m_masterBusId) {
            if (!harkenstudio::IsNull(bus.parentId)) {
                Fail(BusTopologyOperation::ApplySnapshot,
                     kEngineInvalidArg, bus.id, bus.parentId,
                     bus.name);
                return false;
            }
            desiredBuses[m_masterBusId] = std::move(bus);
            continue;
        }
        if (!desiredBuses.emplace(bus.id, bus).second) {
            Fail(BusTopologyOperation::ApplySnapshot, kEngineInUse,
                 bus.id, bus.parentId, bus.name);
            return false;
        }
    }

    for (const auto& entry : desiredBuses) {
        const RuntimeBusDefinition& bus = entry.second;
        if (bus.id == m_masterBusId)
            continue;
        if (desiredBuses.find(bus.parentId) == desiredBuses.end()) {
            Fail(BusTopologyOperation::ApplySnapshot, kEngineUnknownId,
                 bus.id, bus.parentId, bus.name);
            return false;
        }
    }

    std::vector<harkenstudio::ObjectId> parentFirst;
    if (!BuildParentFirstOrder(desiredBuses, parentFirst)) {
        Fail(BusTopologyOperation::ApplySnapshot, kEngineInvalidArg,
             {}, {}, QString());
        return false;
    }

    std::map<SendKey, RuntimeBusSendDefinition> desiredSends;
    for (const RuntimeBusSendDefinition& send : sends) {
        if (harkenstudio::IsNull(send.sourceId) || harkenstudio::IsNull(send.targetId) ||
            desiredBuses.find(send.sourceId) == desiredBuses.end() ||
            desiredBuses.find(send.targetId) == desiredBuses.end()) {
            Fail(BusTopologyOperation::ApplySnapshot, kEngineUnknownId,
                 send.sourceId, send.targetId, QString());
            return false;
        }
        const SendKey key{send.sourceId, send.targetId};
        if (!desiredSends.emplace(key, send).second) {
            Fail(BusTopologyOperation::ApplySnapshot, kEngineInUse,
                 send.sourceId, send.targetId, QString());
            return false;
        }
    }

    m_desiredBuses = std::move(desiredBuses);
    m_desiredSends = std::move(desiredSends);
    return true;
}

bool BusTopologySync::BuildParentFirstOrder(
    const std::map<harkenstudio::ObjectId, RuntimeBusDefinition>& buses,
    std::vector<harkenstudio::ObjectId>& outOrder) const
{
    enum class Visit : uint8_t { None, Visiting, Done };
    std::map<harkenstudio::ObjectId, Visit> visits;
    std::function<bool(harkenstudio::ObjectId)> visit = [&](harkenstudio::ObjectId id) {
        Visit& state = visits[id];
        if (state == Visit::Done)
            return true;
        if (state == Visit::Visiting)
            return false;
        state = Visit::Visiting;
        const auto it = buses.find(id);
        if (it == buses.end())
            return false;
        if (id != m_masterBusId &&
            !visit(it->second.parentId)) {
            return false;
        }
        state = Visit::Done;
        outOrder.push_back(id);
        return true;
    };

    for (const auto& entry : buses) {
        if (!visit(entry.first))
            return false;
    }
    return true;
}

bool BusTopologySync::BuildOperations(uint32_t propertyFadeMs)
{
    if (!m_backend.applySnapshot) {
        Fail(BusTopologyOperation::ApplySnapshot,
             kEngineInvalidArg, {}, {}, QString());
        return false;
    }

    bool topologyChanged =
        m_desiredSends.size() != m_confirmedSends.size()
        || m_desiredBuses.size() != m_confirmedBuses.size();
    if (!topologyChanged) {
        for (const auto& entry : m_desiredBuses) {
            const auto confirmed = m_confirmedBuses.find(entry.first);
            if (confirmed == m_confirmedBuses.end()
                || confirmed->second.parentId != entry.second.parentId) {
                topologyChanged = true;
                break;
            }
        }
    }
    if (!topologyChanged) {
        for (const auto& entry : m_desiredSends) {
            const auto confirmed = m_confirmedSends.find(entry.first);
            if (confirmed == m_confirmedSends.end()
                || !SameFloat(confirmed->second.levelDb,
                              entry.second.levelDb)) {
                topologyChanged = true;
                break;
            }
        }
    }
    if (topologyChanged) {
        Operation operation;
        operation.kind = BusTopologyOperation::ApplySnapshot;
        m_operations.push_back(operation);
        m_pendingRevision = m_confirmedRevision + 1;
    }

    std::vector<harkenstudio::ObjectId> desiredOrder;
    if (!BuildParentFirstOrder(m_desiredBuses, desiredOrder)) {
        Fail(BusTopologyOperation::ApplySnapshot,
             kEngineInvalidArg, {}, {}, QString());
        return false;
    }

    for (const harkenstudio::ObjectId id : desiredOrder) {
        const RuntimeBusDefinition& desired = m_desiredBuses.at(id);
        const auto confirmed = m_confirmedBuses.find(id);
        const RuntimeBusDefinition baseline =
            confirmed == m_confirmedBuses.end()
                ? RuntimeBusDefinition{}
                : confirmed->second;
        if (!SameFloat(baseline.volumeDb, desired.volumeDb)) {
            Operation operation;
            operation.kind = BusTopologyOperation::SetVolume;
            operation.busId = id;
            operation.value = desired.volumeDb;
            operation.fadeMs = propertyFadeMs;
            operation.busName = desired.name;
            m_operations.push_back(operation);
        }
        if (!SameFloat(baseline.outputVolumeDb, desired.outputVolumeDb)) {
            Operation operation;
            operation.kind = BusTopologyOperation::SetOutputVolume;
            operation.busId = id;
            operation.value = desired.outputVolumeDb;
            operation.fadeMs = propertyFadeMs;
            operation.busName = desired.name;
            m_operations.push_back(operation);
        }
        if (baseline.mute != desired.mute) {
            Operation operation;
            operation.kind = BusTopologyOperation::SetMute;
            operation.busId = id;
            operation.flag = desired.mute;
            operation.fadeMs = propertyFadeMs;
            operation.busName = desired.name;
            m_operations.push_back(operation);
        }
        if (baseline.solo != desired.solo) {
            Operation operation;
            operation.kind = BusTopologyOperation::SetSolo;
            operation.busId = id;
            operation.flag = desired.solo;
            operation.fadeMs = propertyFadeMs;
            operation.busName = desired.name;
            m_operations.push_back(operation);
        }
    }
    return true;
}

void BusTopologySync::Pump()
{
    if (m_state != BusTopologyState::Syncing)
        return;

    while (m_nextOperation < m_operations.size()) {
        const Operation& operation = m_operations[m_nextOperation];
        const EngineResult result = Execute(operation);
        if (result == kEngineBackpressure)
            return;
        if (result != kEngineOk) {
            Fail(operation.kind, result, operation.busId,
                 operation.relatedBusId, operation.busName);
            return;
        }
        ApplyConfirmed(operation);
        ++m_nextOperation;
    }

    for (const auto& entry : m_desiredBuses) {
        auto confirmed = m_confirmedBuses.find(entry.first);
        if (confirmed != m_confirmedBuses.end())
            confirmed->second.name = entry.second.name;
    }
    m_propertyReplayOperations.clear();
    m_nextPropertyReplayOperation = 0;
    m_state = BusTopologyState::Ready;
}

void BusTopologySync::BeginConfirmedPropertyReplay(
    const std::vector<ConfirmedBusPropertyPatch>& patches,
    uint32_t propertyFadeMs)
{
    std::map<harkenstudio::ObjectId, RuntimeBusDefinition> desiredById;
    for (std::size_t i = m_nextPropertyReplayOperation;
         i < m_propertyReplayOperations.size(); ++i) {
        const Operation& operation = m_propertyReplayOperations[i];
        const auto confirmed = m_confirmedBuses.find(operation.busId);
        if (confirmed == m_confirmedBuses.end())
            continue;
        RuntimeBusDefinition& desired =
            desiredById.emplace(operation.busId, confirmed->second)
                .first->second;
        switch (operation.kind) {
        case BusTopologyOperation::SetVolume:
            desired.volumeDb = operation.value;
            break;
        case BusTopologyOperation::SetOutputVolume:
            desired.outputVolumeDb = operation.value;
            break;
        case BusTopologyOperation::SetMute:
            desired.mute = operation.flag;
            break;
        case BusTopologyOperation::SetSolo:
            desired.solo = operation.flag;
            break;
        default:
            break;
        }
    }
    for (const ConfirmedBusPropertyPatch& patch : patches) {
        const auto confirmed = m_confirmedBuses.find(patch.busId);
        if (confirmed == m_confirmedBuses.end())
            continue;
        RuntimeBusDefinition& desired =
            desiredById.emplace(patch.busId, confirmed->second)
                .first->second;
        if (!patch.busName.isEmpty())
            desired.name = patch.busName;
        if (patch.volumeDb)
            desired.volumeDb = *patch.volumeDb;
        if (patch.outputVolumeDb)
            desired.outputVolumeDb = *patch.outputVolumeDb;
        if (patch.mute)
            desired.mute = *patch.mute;
        if (patch.solo)
            desired.solo = *patch.solo;
    }

    m_propertyReplayOperations.clear();
    m_nextPropertyReplayOperation = 0;

    for (const auto& entry : m_confirmedBuses) {
        const auto desired = desiredById.find(entry.first);
        if (desired == desiredById.end())
            continue;
        const RuntimeBusDefinition& confirmed = entry.second;
        if (!SameFloat(confirmed.volumeDb, desired->second.volumeDb)) {
            Operation operation;
            operation.kind = BusTopologyOperation::SetVolume;
            operation.busId = entry.first;
            operation.value = desired->second.volumeDb;
            operation.fadeMs = propertyFadeMs;
            operation.busName = desired->second.name;
            m_propertyReplayOperations.push_back(std::move(operation));
        }
        if (!SameFloat(confirmed.outputVolumeDb, desired->second.outputVolumeDb)) {
            Operation operation;
            operation.kind = BusTopologyOperation::SetOutputVolume;
            operation.busId = entry.first;
            operation.value = desired->second.outputVolumeDb;
            operation.fadeMs = propertyFadeMs;
            operation.busName = desired->second.name;
            m_propertyReplayOperations.push_back(std::move(operation));
        }
        if (confirmed.mute != desired->second.mute) {
            Operation operation;
            operation.kind = BusTopologyOperation::SetMute;
            operation.busId = entry.first;
            operation.flag = desired->second.mute;
            operation.fadeMs = propertyFadeMs;
            operation.busName = desired->second.name;
            m_propertyReplayOperations.push_back(std::move(operation));
        }
        if (confirmed.solo != desired->second.solo) {
            Operation operation;
            operation.kind = BusTopologyOperation::SetSolo;
            operation.busId = entry.first;
            operation.flag = desired->second.solo;
            operation.fadeMs = propertyFadeMs;
            operation.busName = desired->second.name;
            m_propertyReplayOperations.push_back(std::move(operation));
        }
    }
}

void BusTopologySync::PumpConfirmedPropertyReplay()
{
    while (m_nextPropertyReplayOperation <
           m_propertyReplayOperations.size()) {
        const Operation& operation =
            m_propertyReplayOperations[m_nextPropertyReplayOperation];
        const EngineResult result = Execute(operation);
        if (result == kEngineBackpressure)
            return;
        if (result != kEngineOk) {
            m_propertyReplayOperations.clear();
            m_nextPropertyReplayOperation = 0;
            return;
        }
        ApplyConfirmed(operation);
        ++m_nextPropertyReplayOperation;
    }
    m_propertyReplayOperations.clear();
    m_nextPropertyReplayOperation = 0;
}

EngineResult BusTopologySync::Execute(const Operation& operation) const
{
    switch (operation.kind) {
    case BusTopologyOperation::ApplySnapshot: {
        std::vector<RuntimeBusDefinition> buses;
        buses.reserve(m_desiredBuses.size());
        for (const auto& entry : m_desiredBuses) {
            if (entry.first != m_masterBusId)
                buses.push_back(entry.second);
        }
        std::vector<RuntimeBusSendDefinition> sends;
        sends.reserve(m_desiredSends.size());
        for (const auto& entry : m_desiredSends)
            sends.push_back(entry.second);
        return m_backend.applySnapshot(
            buses, sends, m_confirmedRevision, m_pendingRevision);
    }
    case BusTopologyOperation::SetVolume:
        return m_backend.setVolume(operation.busId,
                                   operation.value,
                                   operation.fadeMs);
    case BusTopologyOperation::SetOutputVolume:
        return m_backend.setOutputVolume(operation.busId,
                                         operation.value,
                                         operation.fadeMs);
    case BusTopologyOperation::SetMute:
        return m_backend.setMute(operation.busId,
                                 operation.flag,
                                 operation.fadeMs);
    case BusTopologyOperation::SetSolo:
        return m_backend.setSolo(operation.busId,
                                 operation.flag,
                                 operation.fadeMs);
    case BusTopologyOperation::None:
        break;
    }
    return kEngineInvalidArg;
}

void BusTopologySync::ApplyConfirmed(const Operation& operation)
{
    switch (operation.kind) {
    case BusTopologyOperation::ApplySnapshot: {
        std::map<harkenstudio::ObjectId, RuntimeBusDefinition> confirmed;
        for (const auto& entry : m_desiredBuses) {
            RuntimeBusDefinition bus = entry.second;
            const auto old = m_confirmedBuses.find(entry.first);
            if (old != m_confirmedBuses.end()) {
                bus.volumeDb = old->second.volumeDb;
                bus.outputVolumeDb = old->second.outputVolumeDb;
                bus.mute = old->second.mute;
                bus.solo = old->second.solo;
            } else {
                bus.volumeDb = 0.0f;
                bus.outputVolumeDb = 0.0f;
                bus.mute = false;
                bus.solo = false;
            }
            confirmed.emplace(entry.first, std::move(bus));
        }
        m_confirmedBuses = std::move(confirmed);
        m_confirmedSends = m_desiredSends;
        m_confirmedRevision = m_pendingRevision;
        break;
    }
    case BusTopologyOperation::SetVolume:
        m_confirmedBuses.at(operation.busId).volumeDb =
            operation.value;
        break;
    case BusTopologyOperation::SetOutputVolume:
        m_confirmedBuses.at(operation.busId).outputVolumeDb =
            operation.value;
        break;
    case BusTopologyOperation::SetMute:
        m_confirmedBuses.at(operation.busId).mute =
            operation.flag;
        break;
    case BusTopologyOperation::SetSolo:
        m_confirmedBuses.at(operation.busId).solo =
            operation.flag;
        break;
    case BusTopologyOperation::None:
        break;
    }
}

void BusTopologySync::Fail(BusTopologyOperation operation,
                           EngineResult result, harkenstudio::ObjectId busId,
                           harkenstudio::ObjectId relatedBusId,
                           const QString& busName)
{
    m_failure.operation = operation;
    m_failure.result = result;
    m_failure.busId = busId;
    m_failure.relatedBusId = relatedBusId;
    m_failure.busName = busName;
    m_state = BusTopologyState::Failed;
}

std::vector<harkenstudio::ObjectId> BusTopologySync::ConfirmedBusIds() const
{
    std::vector<harkenstudio::ObjectId> ids;
    ids.reserve(m_confirmedBuses.size());
    for (const auto& entry : m_confirmedBuses)
        ids.push_back(entry.first);
    return ids;
}

std::vector<RuntimeBusDefinition> BusTopologySync::ConfirmedBuses() const
{
    std::vector<RuntimeBusDefinition> buses;
    buses.reserve(m_confirmedBuses.size());
    for (const auto& entry : m_confirmedBuses)
        buses.push_back(entry.second);
    return buses;
}

} // namespace Detail
} // namespace Bridge
} // namespace HarkenEditor
