#include "AudioEngine.h"
#include "RuntimeBridge/PreviewBridge.h"

#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "RuntimeSyncCoordinator.h"
#include "VariableRehearsal.h"
#include "VariableSemantics.h"

#include <QDebug>

#include <algorithm>

namespace harken::studio {

AudioEngine::PreviewPlayableStartResult AudioEngine::playPreviewPlayable(
    uint32_t playableKind, harken::studio::types::ObjectId stableId, quint64 emitterHandle)
{
    PreviewPlayableStartResult result;
    if (!session_) {
        result.status = harken::studio::bridge::kPreviewHostError;
        return result;
    }
    const auto barrier = currentWorldPlaybackBarrier();
    if (barrier.state != RuntimeSyncCoordinator::RevisionBarrierState::Ready) {
        result.status = barrier.state == RuntimeSyncCoordinator::RevisionBarrierState::Pending
            ? harken::studio::bridge::kPreviewBackpressure : harken::studio::bridge::kPreviewHostError;
        return result;
    }
    if (emitterHandle == 0 && !harken::studio::types::IsNull(stableId)
        && playableKind <= harken::studio::bridge::kPreviewPlayableSegment) {
        emitterHandle = ensurePreviewEmitter();
        if (emitterHandle == 0) {
            result.status = harken::studio::bridge::kPreviewHostError;
            return result;
        }
        applyPreviewVariableOverrides();
    }

    uint64_t handle = 0;
    result.status = harken::studio::bridge::playPreviewPlayable(playableKind, stableId, emitterHandle, handle);
    result.handle = handle;
    if (result.status != harken::studio::bridge::kPreviewOk
        && result.status != harken::studio::bridge::kPreviewBackpressure) {
        qWarning() << "[Preview] PlayPlayable refused:"
                   << "kind=" << playableKind
                   << "stableId=" << stableId
                   << "emitter=" << emitterHandle
                   << "status=" << result.status;
    }
    return result;
}

int32_t AudioEngine::resetPreviewPlayable(uint32_t playableKind, harken::studio::types::ObjectId stableId)
{
    if (playableKind != harken::studio::bridge::kPreviewPlayableContainer || harken::studio::types::IsNull(stableId))
        return harken::studio::bridge::kPreviewInvalidArg;
    if (!session_)
        return harken::studio::bridge::kPreviewHostError;

    const auto barrier = currentWorldPlaybackBarrier();
    if (barrier.state != RuntimeSyncCoordinator::RevisionBarrierState::Ready)
        return barrier.state == RuntimeSyncCoordinator::RevisionBarrierState::Pending
            ? harken::studio::bridge::kPreviewBackpressure : harken::studio::bridge::kPreviewHostError;

    quint64 emitterHandle = previewEmitterHandle_;
    if (emitterHandle == 0) {
        emitterHandle = ensurePreviewEmitter();
        if (emitterHandle == 0)
            return harken::studio::bridge::kPreviewHostError;
    }

    return harken::studio::bridge::resetPreviewPlayable(playableKind, stableId, emitterHandle);
}

quint64 AudioEngine::ensurePreviewEmitter(harken::studio::bridge::EngineResult* outResult)
{
    if (outResult != nullptr)
        *outResult = harken::studio::bridge::kEngineOk;
    if (previewEmitterHandle_ != 0)
        return previewEmitterHandle_;

    const auto emitter = harken::studio::bridge::createPreviewEmitter(outResult);
    if (emitter == 0)
        return 0;
    previewEmitterHandle_ = emitter;
    applyPreviewVariableOverrides();
    return previewEmitterHandle_;
}

void AudioEngine::releasePreviewEmitter()
{
    if (previewEmitterHandle_ == 0)
        return;
    harken::studio::bridge::destroyPreviewEmitter(previewEmitterHandle_);
    previewEmitterHandle_ = 0;
    for (auto it = previewVariableOverrides_.begin();
         it != previewVariableOverrides_.end();) {
        if (!it->hasValue) {
            it = previewVariableOverrides_.erase(it);
            continue;
        }
        it->acceptedEmitter = 0;
        it->acceptedGlobal = false;
        it->inFlight = false;
        ++it;
    }
}

void AudioEngine::capturePreviewVariableOverride(const QUuid& variableId)
{
    if (variableId.isNull() || variableRehearsal_ == nullptr)
        return;

    const auto rehearsal = variableRehearsal_->overrideFor(variableId);
    if (!rehearsal.has_value()) {
        previewVariableOverrides_.remove(variableId);
        return;
    }
    auto& state = previewVariableOverrides_[variableId];
    state.hasValue = rehearsal.has_value();
    state.value = rehearsal.value_or(0.0f);
    state.acceptedEmitter = 0;
    state.acceptedGlobal = false;
    state.mustResubmit = true;
    state.scope = variableRehearsal_->scopeFor(variableId);

    const ProjectItem* item = ProjectManager::instance().findItemByGuid(variableId);
    const VVariable* variable = item ? item->comp<VVariable>() : nullptr;
    if (variable) {
        AudioEngine::RehearsalPushRecord record{
            variableId,
            state.scope == VariableScope::Global
                ? RehearsalPushPath::Global : RehearsalPushPath::Emitter,
            VariableSemantics::currentValue(
                rehearsal, variable->kind, variable->rangeMin,
                variable->rangeMax, variable->defaultValue),
            false};
        auto existing = std::find_if(
            rehearsalPushLog_.begin(), rehearsalPushLog_.end(),
            [&variableId](const RehearsalPushRecord& entry) {
                return entry.variableId == variableId;
            });
        if (existing != rehearsalPushLog_.end())
            *existing = record;
        else
            rehearsalPushLog_.push_back(record);
        if (rehearsalPushLog_.size() > kRehearsalPushLogCap)
            rehearsalPushLog_.erase(
                rehearsalPushLog_.begin(),
                rehearsalPushLog_.begin() + static_cast<std::ptrdiff_t>(
                    rehearsalPushLog_.size() - kRehearsalPushLogCap));
    }

    applyPreviewVariableOverrides();
}

void AudioEngine::invalidatePreviewVariableAcceptance()
{
    for (auto it = previewVariableOverrides_.begin();
         it != previewVariableOverrides_.end(); ++it) {
        it->acceptedEmitter = 0;
        it->acceptedGlobal = false;
        it->mustResubmit = true;
        it->inFlight = false;
        it->inFlightSequence = 0;
        it->inFlightWorld = 0;
        it->inFlightTransport = 0;
    }
}

void AudioEngine::applyPreviewVariableOverrides()
{
    if (previewEmitterHandle_ == 0 || !session_ || !runtimeSessionActive_)
        return;

    const auto submitWrite = [this](const ProjectItem& item,
                                    const VVariable& variable,
                                    PreviewVariableOverrideState& state) {
        const float value = VariableSemantics::currentValue(
            state.hasValue ? std::optional<float>(state.value) : std::nullopt,
            variable.kind, variable.rangeMin, variable.rangeMax,
            variable.defaultValue);
        harken::studio::bridge::VariableTicket ticket{};
        state.acceptedEmitter = 0;
        state.acceptedGlobal = false;
        state.mustResubmit = true;
        const auto result = harken::studio::bridge::writePreviewVariable(
            item.runtimeId(), value,
            state.scope == VariableScope::Global ? 0 : previewEmitterHandle_, ticket);
        if (result != harken::studio::bridge::kEngineOk)
            return result;
        state.submittedValue = value;
        state.submittedHasValue = state.hasValue;
        state.mustResubmit = false;
        state.inFlight = true;
        state.inFlightSequence = ticket.sequence;
        state.inFlightWorld = ticket.worldGeneration;
        state.inFlightTransport = ticket.transportGeneration;
        return result;
    };

    auto& projects = ProjectManager::instance();
    for (auto it = previewVariableOverrides_.begin();
         it != previewVariableOverrides_.end();) {
        auto& state = it.value();
        if (state.inFlight) {
            const auto outcome = harken::studio::bridge::queryVariableWriteState(
                {state.inFlightSequence, state.inFlightWorld, state.inFlightTransport});
            if (!state.mustResubmit
                && outcome == harken::studio::bridge::VariableWriteState::Pending) {
                ++it;
                continue;
            }
            if (!state.mustResubmit
                && outcome == harken::studio::bridge::VariableWriteState::Applied) {
                if (state.scope == VariableScope::Global)
                    state.acceptedGlobal = true;
                else
                    state.acceptedEmitter = previewEmitterHandle_;
            } else {
                state.acceptedGlobal = false;
                state.acceptedEmitter = 0;
                state.mustResubmit = true;
            }
            state.inFlight = false;
            state.inFlightSequence = 0;
            state.inFlightWorld = 0;
            state.inFlightTransport = 0;
        }

        const ProjectItem* item = projects.findItemByGuid(it.key());
        const VVariable* variable = item != nullptr
            ? item->comp<VVariable>() : nullptr;
        if (variable == nullptr || harken::studio::types::IsNull(item->runtimeId())) {
            it = previewVariableOverrides_.erase(it);
            continue;
        }
        const bool accepted =
            (state.scope == VariableScope::Global && state.acceptedGlobal)
            || (state.scope != VariableScope::Global
                && state.acceptedEmitter == previewEmitterHandle_);
        const float value = VariableSemantics::currentValue(
            state.hasValue ? std::optional<float>(state.value) : std::nullopt,
            variable->kind, variable->rangeMin, variable->rangeMax,
            variable->defaultValue);
        if (!state.mustResubmit && accepted
            && state.submittedHasValue == state.hasValue
            && state.submittedValue == value) {
            ++it;
            continue;
        }
        const harken::studio::bridge::EngineResult result = submitWrite(*item, *variable, state);
        if (result == harken::studio::bridge::kEngineStaleHandle) {
            previewEmitterHandle_ = 0;
            for (auto reset = previewVariableOverrides_.begin();
                 reset != previewVariableOverrides_.end(); ++reset) {
                reset->acceptedEmitter = 0;
                reset->acceptedGlobal = false;
                reset->mustResubmit = true;
                reset->inFlight = false;
            }
            return;
        } else if (result != harken::studio::bridge::kEngineOk &&
                   result != harken::studio::bridge::kEngineBackpressure && result != harken::studio::bridge::kEngineNoVariable) {
            qWarning() << "[Preview] Variable rehearsal sync refused:"
                       << "variable=" << item->runtimeId()
                       << "status=" << result;
        }
        ++it;
    }
}

bool AudioEngine::hasPendingPreviewVariableOverrides() const
{
    for (auto it = previewVariableOverrides_.cbegin();
         it != previewVariableOverrides_.cend(); ++it) {
        if (previewEmitterHandle_ == 0 || it->mustResubmit || it->inFlight)
            return true;
        if (it->scope == VariableScope::Global) {
            if (!it->acceptedGlobal)
                return true;
        } else if (it->acceptedEmitter != previewEmitterHandle_) {
            return true;
        }
    }
    return false;
}

int32_t AudioEngine::stopPreviewPlayable(quint64 handle, uint32_t fadeMs)
{
    return harken::studio::bridge::stopPreviewPlayable(handle, fadeMs);
}

int32_t AudioEngine::queryPreviewPlayable(
    quint64 handle, PreviewPlayableState& outState) const
{
    return harken::studio::bridge::queryPreviewPlayable(handle, outState);
}

}
