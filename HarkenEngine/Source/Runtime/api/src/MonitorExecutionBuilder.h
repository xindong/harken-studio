#ifndef HARKEN_API_MONITOR_EXECUTION_BUILDER_H
#define HARKEN_API_MONITOR_EXECUTION_BUILDER_H

#include <harken/Harken.h>
#include <harken/core/MonitorEmitterIdentity.h>
#include <harken/core/MonitorExecutionFact.h>
#include <harken/core/MonitorRequest.h>

namespace harken::monitor {

// Call facts reuse the otherwise inactive execution-request bits for targets.
// Public validity indices never change; this mapping is private to source PODs.
constexpr int SourceFieldIndex(MonitorFactBody body, unsigned field) noexcept {
    if (body == MonitorFactBody::Call) {
        if (field >= VN_MONITOR_FACT_FIELD_TARGET_HANDLE && field <= VN_MONITOR_FACT_FIELD_TARGET_SCOPE)
            return static_cast<int>(field - VN_MONITOR_FACT_FIELD_TARGET_HANDLE + 7);
        if ((field >= VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST && field <= VN_MONITOR_FACT_FIELD_PARENT_ACTION) ||
            (field >= VN_MONITOR_FACT_FIELD_SERVICE_LEAF && field <= VN_MONITOR_FACT_FIELD_RENDER_ACTIVATION))
            return -1;
        return field < 64 ? static_cast<int>(field) : -1;
    }
    if (field >= VN_MONITOR_FACT_FIELD_ACTIVITY_STEP && field <= VN_MONITOR_FACT_FIELD_ACTION_OUTCOME)
        return static_cast<int>(field - VN_MONITOR_FACT_FIELD_ACTIVITY_STEP + 58);
    return field <= VN_MONITOR_FACT_FIELD_TIMESTAMP ? static_cast<int>(field) : -1;
}

inline bool SourceHas(const MonitorExecutionFact& fact, unsigned field) noexcept {
    const auto index = SourceFieldIndex(fact.BodyKind(), field);
    return index >= 0 && (fact.validity & (UINT64_C(1) << index)) != 0;
}

inline bool SetSourceField(MonitorExecutionFact& fact, unsigned field, bool valid = true) noexcept {
    const auto index = SourceFieldIndex(fact.BodyKind(), field);
    if (index < 0) return false;
    const auto bit = UINT64_C(1) << index;
    fact.validity = valid ? fact.validity | bit : fact.validity & ~bit;
    return true;
}

constexpr std::uint64_t SourceAllowedFields(MonitorFactBody body) noexcept {
    std::uint64_t bits = 0;
    for (unsigned field = 0; field <= VN_MONITOR_FACT_FIELD_ACTION_OUTCOME; ++field) {
        const auto index = SourceFieldIndex(body, field);
        if (index >= 0) bits |= UINT64_C(1) << index;
    }
    return bits;
}

constexpr bool IsCallFactKind(unsigned kind) noexcept {
    return kind == VN_MONITOR_FACT_REQUEST_SUBMITTED || kind == VN_MONITOR_FACT_REQUEST_RETURNED ||
        kind == VN_MONITOR_FACT_REQUEST_TERMINAL;
}

constexpr bool IsActivityFactKind(unsigned kind) noexcept {
    return kind == VN_MONITOR_FACT_STEP_EXECUTED || kind == VN_MONITOR_FACT_VARIABLE_RESULT ||
        kind == VN_MONITOR_FACT_ACTION_TERMINAL;
}

constexpr std::uint64_t SubjectGeneration(unsigned subject, std::uint64_t handle) noexcept {
    return subject == VN_MONITOR_SUBJECT_ACTION ? handle >> 12 : handle >> 32;
}

constexpr bool ValidSubjectHandle(unsigned subject, std::uint64_t handle) noexcept {
    if (subject == VN_MONITOR_SUBJECT_NONE || subject > VN_MONITOR_SUBJECT_TRANSITION ||
        handle == 0 || SubjectGeneration(subject, handle) == 0) return false;
    // Public Preview tagging is only meaningful for a request target. Internal
    // Playable and LogicalVoice handles reserve that bit and never contain it.
    return (subject != VN_MONITOR_SUBJECT_PLAYABLE && subject != VN_MONITOR_SUBJECT_LOGICAL_VOICE) ||
        (handle & (UINT64_C(1) << 63)) == 0;
}

static_assert(VN_MONITOR_OPERATION_STOP_EMITTER < 256);
static_assert(kMonitorOperationResetPreview < 256 && kMonitorOperationPlayRawPreview < 256);
static_assert(static_cast<unsigned>(MonitorRequestOrigin::StudioPreview) < 256);
static_assert(static_cast<unsigned>(MonitorEmitterKind::ExplicitSpatial) < 256);
static_assert(static_cast<unsigned>(MonitorHostOwnership::StudioDefaultEmitter) < 256);
static_assert(static_cast<unsigned>(MonitorBindingState::Detached) < 16);

inline bool ValidRequestOperation(std::uint32_t value) noexcept {
    return value >= VN_MONITOR_OPERATION_POST_ACTION && value <= VN_MONITOR_OPERATION_RESET_VALUES;
}

inline bool ValidRequestOrigin(std::uint32_t value) noexcept {
    return value > 0 && value <= static_cast<unsigned>(MonitorRequestOrigin::StudioPreview);
}

// Each member has independent validity. A foreign/exhausted key does not erase
// an operation, origin or internal action cause that is still known.
inline MonitorFactRequest CaptureFactRequest(MonitorExecutionFact& fact,
    const MonitorRequestContext& context, unsigned requestField, unsigned operationField, unsigned originField) noexcept {
    MonitorFactRequest result{};
    SetSourceField(fact, requestField, false);
    SetSourceField(fact, operationField, false);
    SetSourceField(fact, originField, false);
    if (context.key.Valid() && context.key.engineInstance == fact.engine) {
        result.sequence = context.key.sequence;
        SetSourceField(fact, requestField);
    } else if (context.operation != 0 || context.key.sequence != 0 || context.key.engineInstance != 0) {
        fact.flags |= kMonitorFactIdentityIncomplete;
    }
    if (ValidRequestOperation(context.operation)) {
        result.operation = context.operation;
        SetSourceField(fact, operationField);
    } else if (context.operation != 0) fact.flags |= kMonitorFactIdentityIncomplete;
    const auto origin = static_cast<std::uint32_t>(context.origin);
    if (ValidRequestOrigin(origin)) {
        result.origin = origin;
        SetSourceField(fact, originField);
    } else if (origin != 0) fact.flags |= kMonitorFactIdentityIncomplete;
    return result;
}

inline bool CaptureCallRequests(MonitorExecutionFact& fact, const MonitorRequestContext& request,
    const MonitorRequestContext& causedBy = {}) noexcept {
    if (fact.BodyKind() != MonitorFactBody::Call || !IsCallFactKind(fact.kind)) return false;
    fact.body.call.request = CaptureFactRequest(fact, request, VN_MONITOR_FACT_FIELD_REQUEST,
        VN_MONITOR_FACT_FIELD_REQUEST_OPERATION, VN_MONITOR_FACT_FIELD_REQUEST_ORIGIN);
    fact.body.call.causedBy = CaptureFactRequest(fact, causedBy, VN_MONITOR_FACT_FIELD_CAUSED_BY_REQUEST,
        VN_MONITOR_FACT_FIELD_CAUSED_BY_OPERATION, VN_MONITOR_FACT_FIELD_CAUSED_BY_ORIGIN);
    return true;
}

inline bool CaptureExecutionRequests(MonitorExecutionFact& fact, const MonitorRequestContext& playback,
    const MonitorControlCause& control, const MonitorControlCause& selection) noexcept {
    if (fact.BodyKind() != MonitorFactBody::Execution) return false;
    auto& requests = fact.body.execution.requests;
    requests = {};
    for (unsigned i = VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST; i <= VN_MONITOR_FACT_FIELD_SELECTION_ORDINAL; ++i)
        SetSourceField(fact, i, false);
    const auto copyRequest = [&](const MonitorRequestContext& request, unsigned field,
        std::uint64_t& sequence, std::uint8_t& operation, std::uint8_t& origin) {
        const auto captured = CaptureFactRequest(fact, request, field, field + 1, field + 2);
        sequence = captured.sequence;
        operation = static_cast<std::uint8_t>(captured.operation);
        origin = static_cast<std::uint8_t>(captured.origin);
    };
    copyRequest(playback, VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST,
        requests.playback, requests.playbackOperation, requests.playbackOrigin);
    copyRequest(control.request, VN_MONITOR_FACT_FIELD_CONTROL_REQUEST,
        requests.control, requests.controlOperation, requests.controlOrigin);
    copyRequest(selection.request, VN_MONITOR_FACT_FIELD_SELECTION_REQUEST,
        requests.selection, requests.selectionOperation, requests.selectionOrigin);
    const auto copyCause = [&](const MonitorControlCause& cause, unsigned field,
        std::uint64_t& action, std::uint64_t& parent, std::uint32_t& ordinal) {
        if (ValidSubjectHandle(VN_MONITOR_SUBJECT_ACTION, cause.actionFiring)) {
            action = cause.actionFiring; SetSourceField(fact, field);
        } else if (cause.actionFiring != 0) fact.flags |= kMonitorFactIdentityIncomplete;
        if (ValidSubjectHandle(VN_MONITOR_SUBJECT_PLAYABLE, cause.parentPlayable)) {
            parent = cause.parentPlayable; SetSourceField(fact, field + 1);
        } else if (cause.parentPlayable != 0) fact.flags |= kMonitorFactIdentityIncomplete;
        if (cause.executionOrdinal != UINT32_MAX) { ordinal = cause.executionOrdinal; SetSourceField(fact, field + 2); }
    };
    copyCause(control, VN_MONITOR_FACT_FIELD_CONTROL_ACTION,
        requests.controlAction, requests.controlParent, requests.controlOrdinal);
    copyCause(selection, VN_MONITOR_FACT_FIELD_SELECTION_ACTION,
        requests.selectionAction, requests.selectionParent, requests.selectionOrdinal);
    return true;
}

inline void CaptureFactEmitters(MonitorExecutionFact& fact, const MonitorEmitterOrigin& origin,
    const MonitorEmitterBinding& binding) noexcept {
    auto& emitters = fact.BodyKind() == MonitorFactBody::Call ? fact.body.call.emitters : fact.body.execution.emitters;
    emitters = {};
    fact.originKind = 0;
    fact.hostOwnership = 0;
    fact.bindingStates = 0;
    for (unsigned i = VN_MONITOR_FACT_FIELD_ORIGIN_EMITTER; i <= VN_MONITOR_FACT_FIELD_SPATIAL_BINDING; ++i)
        SetSourceField(fact, i, false);
    if (origin.emitter != 0) {
        emitters.origin = origin.emitter;
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_ORIGIN_EMITTER);
    }
    if (origin.kind != MonitorEmitterKind::Unknown && origin.kind <= MonitorEmitterKind::ExplicitSpatial) {
        fact.originKind = static_cast<std::uint8_t>(origin.kind);
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_ORIGIN_KIND);
    }
    if (origin.host.Valid() && origin.host.engineInstance == fact.engine) {
        emitters.hostSession = origin.host.sdkSession;
        emitters.hostSequence = origin.host.localSequence;
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_HOST_KEY);
        if (origin.registrationRevision != 0) {
            emitters.hostRevision = origin.registrationRevision;
            SetSourceField(fact, VN_MONITOR_FACT_FIELD_HOST_REVISION);
        }
    } else if (origin.host.engineInstance != 0 || origin.host.sdkSession != 0 || origin.host.localSequence != 0)
        fact.flags |= kMonitorFactIdentityIncomplete;
    if (origin.ownership != MonitorHostOwnership::Unknown && origin.ownership <= MonitorHostOwnership::StudioDefaultEmitter) {
        fact.hostOwnership = static_cast<std::uint8_t>(origin.ownership);
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_HOST_OWNERSHIP);
    }
    const auto copyBinding = [&](MonitorBindingState state, std::uint64_t emitter, unsigned field, unsigned shift) {
        if (state > MonitorBindingState::Detached || (state == MonitorBindingState::Bound && emitter == 0)) {
            fact.flags |= kMonitorFactIdentityIncomplete;
            return;
        }
        fact.bindingStates |= static_cast<std::uint8_t>(static_cast<unsigned>(state) << shift);
        SetSourceField(fact, field);
        if (state == MonitorBindingState::Bound) {
            if (shift == 0) emitters.currentState = emitter;
            else emitters.currentSpatial = emitter;
            SetSourceField(fact, shift == 0 ? VN_MONITOR_FACT_FIELD_CURRENT_STATE_EMITTER : VN_MONITOR_FACT_FIELD_CURRENT_SPATIAL_EMITTER);
        }
    };
    copyBinding(binding.state, binding.stateEmitter, VN_MONITOR_FACT_FIELD_STATE_BINDING, 0);
    copyBinding(binding.spatial, binding.spatialEmitter, VN_MONITOR_FACT_FIELD_SPATIAL_BINDING, 4);
}

} // namespace harken::monitor

#endif
