#pragma once

#include "Utils/ObjectId.h"

#include "PlaybackBridge.h"

#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace harken::studio {
namespace bridge {
namespace Detail {

struct BusTopologyBackend {
    std::function<EngineResult(
        const std::vector<RuntimeBusDefinition>&,
        const std::vector<RuntimeBusSendDefinition>&,
        uint64_t, uint64_t)> applySnapshot;
    std::function<EngineResult(harken::studio::types::ObjectId, float, uint32_t)> setVolume;
    std::function<EngineResult(harken::studio::types::ObjectId, bool, uint32_t)> setMute;
    std::function<EngineResult(harken::studio::types::ObjectId, bool, uint32_t)> setSolo;
    std::function<EngineResult(harken::studio::types::ObjectId, float, uint32_t)> setOutputVolume;
};

struct ConfirmedBusPropertyPatch {
    harken::studio::types::ObjectId busId = {};
    QString busName;
    std::optional<float> volumeDb;
    std::optional<float> outputVolumeDb;
    std::optional<bool> mute;
    std::optional<bool> solo;
};

class BusTopologySync {
public:
    explicit BusTopologySync(BusTopologyBackend backend,
                             harken::studio::types::ObjectId masterBusId);

    void Submit(const std::vector<RuntimeBusDefinition>& buses,
                const std::vector<RuntimeBusSendDefinition>& sends,
                uint32_t propertyFadeMs = 0);
    void ConfirmWithoutApply(const std::vector<RuntimeBusDefinition>& buses,
                             const std::vector<RuntimeBusSendDefinition>& sends);
    void Pump();
    void BeginConfirmedPropertyReplay(
        const std::vector<ConfirmedBusPropertyPatch>& patches,
        uint32_t propertyFadeMs = 0);
    void PumpConfirmedPropertyReplay();
    void Reset();

    BusTopologyState State() const { return m_state; }
    const BusTopologyFailure& Failure() const { return m_failure; }
    std::vector<harken::studio::types::ObjectId> ConfirmedBusIds() const;
    std::vector<RuntimeBusDefinition> ConfirmedBuses() const;
    bool ConfirmedPropertyReplayPending() const {
        return m_nextPropertyReplayOperation <
               m_propertyReplayOperations.size();
    }

private:
    struct SendKey {
        harken::studio::types::ObjectId sourceId = {};
        harken::studio::types::ObjectId targetId = {};

        bool operator<(const SendKey& other) const {
            return std::tie(sourceId, targetId) <
                   std::tie(other.sourceId, other.targetId);
        }
    };

    struct Operation {
        BusTopologyOperation kind = BusTopologyOperation::None;
        harken::studio::types::ObjectId busId = {};
        harken::studio::types::ObjectId relatedBusId = {};
        float value = 0.0f;
        bool flag = false;
        uint32_t fadeMs = 0;
        QString busName;
    };

    bool BuildDesired(
        const std::vector<RuntimeBusDefinition>& buses,
        const std::vector<RuntimeBusSendDefinition>& sends);
    bool BuildOperations(uint32_t propertyFadeMs);
    bool BuildParentFirstOrder(
        const std::map<harken::studio::types::ObjectId, RuntimeBusDefinition>& buses,
        std::vector<harken::studio::types::ObjectId>& outOrder) const;
    void ApplyConfirmed(const Operation& operation);
    EngineResult Execute(const Operation& operation) const;
    void Fail(BusTopologyOperation operation, EngineResult result,
              harken::studio::types::ObjectId busId, harken::studio::types::ObjectId relatedBusId,
              const QString& busName);

    BusTopologyBackend m_backend;
    harken::studio::types::ObjectId m_masterBusId = {};
    BusTopologyState m_state = BusTopologyState::Idle;
    BusTopologyFailure m_failure;
    std::map<harken::studio::types::ObjectId, RuntimeBusDefinition> m_desiredBuses;
    std::map<harken::studio::types::ObjectId, RuntimeBusDefinition> m_confirmedBuses;
    std::map<SendKey, RuntimeBusSendDefinition> m_desiredSends;
    std::map<SendKey, RuntimeBusSendDefinition> m_confirmedSends;
    std::vector<Operation> m_operations;
    std::size_t m_nextOperation = 0;
    std::vector<Operation> m_propertyReplayOperations;
    std::size_t m_nextPropertyReplayOperation = 0;
    uint64_t m_confirmedRevision = 0;
    uint64_t m_pendingRevision = 0;
};

} // namespace Detail
} // namespace bridge
} // namespace harken::studio
