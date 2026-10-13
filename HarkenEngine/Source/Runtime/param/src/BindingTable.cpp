// Definition registry and attachment pool of the public binding system.
// Every entry validates exhaustively
// before touching pool state; replaces stage and install at the fixed
// update phase; bus attachments evaluate into the public Variable slot of the
// Variable slot with zero audio-thread involvement.
#include <harken/core/BindingTransition.h>
#include "harken/param/BindingTable.h"

#include <harken/Harken.h>

#include "harken/param/EffectParamLocator.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <bit>
#include <cmath>

namespace harken {

namespace {

// One pool-exhaustion log record naming which pool refused (the
// no-binding refusal's contract: the remedy must be unambiguous).
void LogPoolExhausted(LogRing* log, std::uint64_t pool,
                      std::uint64_t capacity) {
    if (log == nullptr) {
        return;
    }
    LogRecord record{};
    record.level = VN_LOG_WARN;
    record.code = kLogCodeBindingPoolExhausted;
    record.arg0 = pool;
    record.arg1 = capacity;
    log->Push(record);
}

} // namespace

BindingTable::BindingTable(std::uint32_t maxDefinitions,
                           std::uint32_t maxAttachments,
                           std::uint32_t maxBuses)
    : m_definitions(maxDefinitions),
      m_attachments(maxAttachments),
      m_voiceEffectInstances(maxAttachments),
      m_busAccum(static_cast<std::size_t>(maxBuses) * kBusTargetCount, 0.0f),
      m_busAccumActive(static_cast<std::size_t>(maxBuses) * kBusTargetCount,
                       false),
      m_busAccumAccepted(static_cast<std::size_t>(maxBuses) * kBusTargetCount,
                         false) {}

VnResult BindingTable::ValidateDescriptor(std::uint32_t scopeKind,
                                          std::uint32_t targetId,
                                          const CurvePoint* points,
                                          const CurveHandlePair* handles,
                                          std::uint32_t pointCount,
                                          std::uint32_t transitionMs,
                                          Curve* outCurve) {
    // The curve rules are exactly Curve::SetPoints' (finite coordinates,
    // valid Bezier handles, strictly increasing x, 1..8 points); a refused
    // set leaves *outCurve empty and nothing else was touched yet.
    if (!IsValidBindingTransitionMs(transitionMs)
        || !outCurve->SetPoints(points, handles, pointCount)) {
        return VN_INVALID_ARG;
    }
    // Effect-parameter scopes: the targetId is a packed locator, not a target-
    // meta id, so it is validated against the effect-parameter metadata
    // authority instead of the voice/bus target table. The located parameter
    // must exist (known effect type, in-range index) and accept a binding (its
    // combine domain is not None); a transition is refused (effect parameters
    // are block-boundary steps, no ramp field). Object/slot liveness is checked
    // at attach/apply time against the installed effect, not here.
    if (scopeKind == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM ||
        scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
        const EffectParamLocator loc = UnpackEffectParamLocator(targetId);
        const EffectParamMeta* pm =
            FindEffectParamMeta(loc.effectType, loc.paramIndex);
        if (pm == nullptr || pm->combine == EffectParamCombine::None) {
            return VN_INVALID_ARG;
        }
        if (transitionMs != 0) {
            return VN_INVALID_ARG;
        }
        return VN_OK;
    }
    // Known scoped target only — including scope kinds that do not exist
    // yet, refused rather than guessed (section 8).
    const TargetMeta* meta = FindTargetMeta(scopeKind, targetId);
    if (meta == nullptr) {
        return VN_INVALID_ARG;
    }
    // Section 9.1: a nonzero transition on a snap-only target refuses
    // loudly at registration, never silently ignored.
    if (transitionMs != 0 && !meta->supportsTransition) {
        return VN_INVALID_ARG;
    }
    return VN_OK;
}

VnResult BindingTable::CanRegister(BindingId id, VnVariableId variableId,
                                   std::uint32_t scopeKind,
                                   std::uint32_t targetId,
                                   const CurvePoint* points,
                                   const CurveHandlePair* handles,
                                   std::uint32_t pointCount,
                                   std::uint32_t transitionMs) const {
    // The Variable id is structural identity, never validated (any id may
    // drive a binding; an unset Variable simply contributes nothing).
    (void)variableId;
    Curve curve;
    const VnResult valid = ValidateDescriptor(scopeKind, targetId, points,
                                              handles, pointCount,
                                              transitionMs, &curve);
    if (valid != VN_OK) {
        return valid;
    }
    if (Find(id) != nullptr) {
        return VN_IN_USE;
    }
    // The definition count tracks used slots exactly, so a full count is
    // exactly "no free slot" — the same refusal the slot scan produces.
    if (m_definitionCount >= m_definitions.size()) {
        return kResultNoBinding;
    }
    return VN_OK;
}

VnResult BindingTable::RegisterBinding(BindingId id, VnVariableId variableId,
                                       std::uint32_t scopeKind,
                                       std::uint32_t targetId,
                                       const CurvePoint* points,
                                       const CurveHandlePair* handles,
                                       std::uint32_t pointCount,
                                       std::uint32_t transitionMs,
                                       LogRing* log,
                                       std::uint64_t ownerCookie) {
    const VnResult admitted = CanRegister(id, variableId, scopeKind, targetId,
                                          points, handles, pointCount,
                                          transitionMs);
    if (admitted != VN_OK) {
        if (admitted == kResultNoBinding) {
            ++m_definitionRefusedCount;
            LogPoolExhausted(log, kBindingPoolDefinitions,
                             m_definitions.size());
        }
        return admitted;
    }
    // Rebuild the validated curve: the admission proved this set succeeds,
    // and SetPoints is pure, so the admission/commit split shares no
    // scratch state.
    Curve curve;
    if (ValidateDescriptor(scopeKind, targetId, points, handles, pointCount,
                           transitionMs, &curve) != VN_OK) {
        return VN_INVALID_ARG; // unreachable: the admission ran this check
    }
    Definition* slot = nullptr;
    for (Definition& def : m_definitions) {
        if (!def.used) {
            slot = &def;
            break;
        }
    }
    if (slot == nullptr) {
        // Unreachable: CanRegister proved a free slot exists and this
        // thread is the only mutator. Kept defensive (the attach twin).
        ++m_definitionRefusedCount;
        LogPoolExhausted(log, kBindingPoolDefinitions,
                         m_definitions.size());
        return kResultNoBinding;
    }
    *slot = Definition{};
    slot->used = true;
    slot->id = id;
    slot->variableId = variableId;
    slot->scopeKind = scopeKind;
    slot->targetId = targetId;
    slot->curve = curve;
    slot->transitionMs = transitionMs;
    slot->ownerCookie = ownerCookie;
    ++m_definitionCount;
    if (m_definitionCount > m_definitionHighWater) {
        m_definitionHighWater = m_definitionCount;
    }
    return VN_OK;
}

VnResult BindingTable::UnregisterBinding(BindingId id) {
    Definition* def = FindMutable(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (AnyAttachmentOf(id)) {
        return VN_IN_USE;
    }
    *def = Definition{};
    --m_definitionCount;
    return VN_OK;
}

VnResult BindingTable::ReplaceBinding(BindingId id, VnVariableId variableId,
                                      std::uint32_t scopeKind,
                                      std::uint32_t targetId,
                                      const CurvePoint* points,
                                      const CurveHandlePair* handles,
                                      std::uint32_t pointCount,
                                      std::uint32_t transitionMs) {
    Definition* def = FindMutable(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    // Structural identity is frozen: only curve and transitionMs may
    // change; a target move is detach + unregister + register by design.
    if (variableId != def->variableId || scopeKind != def->scopeKind ||
        targetId != def->targetId) {
        return VN_INVALID_ARG;
    }
    Curve curve;
    const VnResult valid = ValidateDescriptor(scopeKind, targetId, points,
                                              handles, pointCount,
                                              transitionMs, &curve);
    if (valid != VN_OK) {
        return valid;
    }
    // Fully validated: stage the revision. It installs at the next
    // update's fixed phase; until then every reader keeps evaluating the
    // current revision. A second same-tick replace overwrites the staged
    // one — the last revision is the one that becomes visible.
    def->pendingCurve = curve;
    def->pendingTransitionMs = transitionMs;
    def->pendingInstall = true;
    return VN_OK;
}

VnResult BindingTable::PrepareRegisterDefinition(
    BindingId id, VnVariableId variableId, std::uint32_t scopeKind,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs, std::uint32_t slotIndex,
    std::uint64_t ownerCookie, PreparedDefinitionToken* out,
    bool slotWillBeVacant, bool idWillBeRemoved) {
    if (out == nullptr || slotIndex >= m_definitions.size()) {
        return VN_INVALID_ARG;
    }
    *out = PreparedDefinitionToken{};
    Curve curve;
    const VnResult valid =
        ValidateDescriptor(scopeKind, targetId, points, handles, pointCount,
                           transitionMs, &curve);
    if (valid != VN_OK) {
        return valid;
    }
    if (Find(id) != nullptr && !idWillBeRemoved) {
        return VN_IN_USE;
    }
    if (m_definitions[slotIndex].used && !slotWillBeVacant) {
        return kResultNoBinding;
    }
    Definition value;
    value.used = true;
    value.id = id;
    value.variableId = variableId;
    value.scopeKind = scopeKind;
    value.targetId = targetId;
    value.curve = curve;
    value.transitionMs = transitionMs;
    value.ownerCookie = ownerCookie;
    out->m_operation = PreparedDefinitionToken::Operation::Add;
    out->m_slotIndex = slotIndex;
    out->m_value = value;
    return VN_OK;
}

VnResult BindingTable::PrepareReplaceDefinition(
    BindingId id, VnVariableId variableId, std::uint32_t scopeKind,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs, PreparedDefinitionToken* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedDefinitionToken{};
    Definition* def = FindMutable(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (variableId != def->variableId || scopeKind != def->scopeKind ||
        targetId != def->targetId) {
        return VN_INVALID_ARG;
    }
    Curve curve;
    const VnResult valid =
        ValidateDescriptor(scopeKind, targetId, points, handles, pointCount,
                           transitionMs, &curve);
    if (valid != VN_OK) {
        return valid;
    }
    Definition value = *def;
    value.pendingCurve = curve;
    value.pendingTransitionMs = transitionMs;
    value.pendingInstall = true;
    out->m_operation = PreparedDefinitionToken::Operation::Replace;
    out->m_slotIndex =
        static_cast<std::uint32_t>(def - m_definitions.data());
    out->m_value = value;
    return VN_OK;
}

VnResult BindingTable::PrepareRemoveDefinition(
    BindingId id, PreparedDefinitionToken* out,
    bool attachmentsWillBeDetached) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedDefinitionToken{};
    Definition* def = FindMutable(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (AnyAttachmentOf(id) && !attachmentsWillBeDetached) {
        return VN_IN_USE;
    }
    out->m_operation = PreparedDefinitionToken::Operation::Remove;
    out->m_slotIndex =
        static_cast<std::uint32_t>(def - m_definitions.data());
    out->m_value = *def;
    return VN_OK;
}

void BindingTable::CancelPreparedDefinition(
    PreparedDefinitionToken* token) noexcept {
    if (token != nullptr) {
        *token = PreparedDefinitionToken{};
    }
}

void BindingTable::CommitPreparedDefinition(
    PreparedDefinitionToken* token, bool installBusRevisionImmediately) noexcept {
    if (token == nullptr || !token->IsPrepared()) {
        return;
    }
    Definition& slot = m_definitions[token->m_slotIndex];
    switch (token->m_operation) {
    case PreparedDefinitionToken::Operation::Add:
        slot = token->m_value;
        ++m_definitionCount;
        if (m_definitionCount > m_definitionHighWater) {
            m_definitionHighWater = m_definitionCount;
        }
        break;
    case PreparedDefinitionToken::Operation::Replace:
        slot = token->m_value;
        if (installBusRevisionImmediately && slot.scopeKind == kTargetScopeBus &&
            slot.pendingInstall) {
            slot.curve = slot.pendingCurve;
            slot.transitionMs = slot.pendingTransitionMs;
            slot.pendingInstall = false;
            ++slot.revision;
        }
        break;
    case PreparedDefinitionToken::Operation::Remove:
        slot = Definition{};
        --m_definitionCount;
        break;
    case PreparedDefinitionToken::Operation::None:
        break;
    }
    *token = PreparedDefinitionToken{};
}

VnResult BindingTable::CanAttach(BindingId id, std::uint32_t scopeKind,
                                 VnObjectId objectId) const {
    const Definition* def = Find(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (scopeKind != def->scopeKind) {
        return VN_INVALID_ARG;
    }
    if (FindAttachment(id, scopeKind, objectId) != nullptr) {
        return VN_IN_USE;
    }
    if (m_attachmentCount >= m_attachments.size()) {
        return kResultNoBinding;
    }
    return VN_OK;
}

VnResult BindingTable::Attach(BindingId id, std::uint32_t scopeKind,
                              VnObjectId objectId, std::uint32_t busIndex,
                              LogRing* log, std::uint64_t ownerCookie) {
    const VnResult admitted = CanAttach(id, scopeKind, objectId);
    if (admitted != VN_OK) {
        if (admitted == kResultNoBinding) {
            ++m_attachmentRefusedCount;
            LogPoolExhausted(log, kBindingPoolAttachments,
                             m_attachments.size());
        }
        return admitted;
    }
    const Definition* def = Find(id);
    for (std::uint32_t i = 0; i < m_attachments.size(); ++i) {
        Attachment& a = m_attachments[i];
        if (a.used) {
            continue;
        }
        a.used = true;
        a.bindingId = id;
        a.definitionIndex =
            static_cast<std::uint32_t>(def - m_definitions.data());
        a.scopeKind = scopeKind;
        a.objectId = objectId;
        a.busIndex = busIndex;
        a.ownerCookie = ownerCookie;
        ++m_attachmentCount;
        if (m_attachmentCount > m_attachmentHighWater) {
            m_attachmentHighWater = m_attachmentCount;
        }
        return VN_OK;
    }
    // Unreachable: CanAttach proved a free slot exists and this thread is
    // the only mutator. Kept defensive.
    ++m_attachmentRefusedCount;
    LogPoolExhausted(log, kBindingPoolAttachments, m_attachments.size());
    return kResultNoBinding;
}

VnResult BindingTable::Detach(BindingId id, std::uint32_t scopeKind,
                              VnObjectId objectId, BusTargetStore* store) {
    for (Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id && a.scopeKind == scopeKind &&
            a.objectId == objectId) {
            // A departing BUS contribution ramps back to neutral over the
            // definition's transitionMs (section 5.3): mark the target
            // dirty with that ramp folded, so the next publication carries
            // the exit. An attachment that contributed nothing (unset
            // Variable, frozen) departs silently — nothing to ramp.
            if (store != nullptr && a.scopeKind == kTargetScopeBus &&
                a.lastHadValue) {
                const Definition& def = m_definitions[a.definitionIndex];
                store->MarkVariableChanged(a.busIndex, def.targetId,
                                           def.transitionMs);
            }
            a = Attachment{};
            --m_attachmentCount;
            return VN_OK;
        }
    }
    return VN_UNKNOWN_ID;
}

VnResult BindingTable::PrepareAttach(
    BindingId id, std::uint32_t scopeKind, VnObjectId objectId,
    std::uint32_t busIndex, std::uint32_t slotIndex,
    std::uint64_t ownerCookie, PreparedAttachmentToken* out,
    std::uint32_t preparedDefinitionIndex,
    bool definitionWillBeAdded, bool slotWillBeVacant,
    bool duplicateWillBeDetached) {
    if (out == nullptr || slotIndex >= m_attachments.size()) {
        return VN_INVALID_ARG;
    }
    *out = PreparedAttachmentToken{};
    const Definition* def = Find(id);
    if (def == nullptr && !definitionWillBeAdded) {
        return VN_UNKNOWN_ID;
    }
    if (def != nullptr && !definitionWillBeAdded &&
        scopeKind != def->scopeKind) {
        return VN_INVALID_ARG;
    }
    if (FindAttachment(id, scopeKind, objectId) != nullptr &&
        !duplicateWillBeDetached) {
        return VN_IN_USE;
    }
    if (m_attachments[slotIndex].used && !slotWillBeVacant) {
        return kResultNoBinding;
    }
    Attachment value;
    value.used = true;
    value.bindingId = id;
    value.definitionIndex =
        !definitionWillBeAdded && def != nullptr
            ? static_cast<std::uint32_t>(def - m_definitions.data())
            : preparedDefinitionIndex;
    value.scopeKind = scopeKind;
    value.objectId = objectId;
    value.busIndex = busIndex;
    value.ownerCookie = ownerCookie;
    out->m_operation = PreparedAttachmentToken::Operation::Add;
    out->m_slotIndex = slotIndex;
    out->m_value = value;
    return VN_OK;
}

VnResult BindingTable::PrepareDetach(
    BindingId id, std::uint32_t scopeKind, VnObjectId objectId,
    PreparedAttachmentToken* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedAttachmentToken{};
    for (std::uint32_t i = 0; i < m_attachments.size(); ++i) {
        const Attachment& attachment = m_attachments[i];
        if (attachment.used && attachment.bindingId == id &&
            attachment.scopeKind == scopeKind &&
            attachment.objectId == objectId) {
            out->m_operation = PreparedAttachmentToken::Operation::Remove;
            out->m_slotIndex = i;
            out->m_value = attachment;
            return VN_OK;
        }
    }
    return VN_UNKNOWN_ID;
}

bool BindingTable::CanCommitPreparedAttachment(
    const PreparedAttachmentToken& token) const noexcept {
    if (token.m_slotIndex >= m_attachments.size()) return false;
    const Attachment& current = m_attachments[token.m_slotIndex];
    if (token.m_operation == PreparedAttachmentToken::Operation::Add) {
        return !current.used;
    }
    if (token.m_operation != PreparedAttachmentToken::Operation::Remove) return false;
    const Attachment& expected = token.m_value;
    return current.used && expected.used &&
        current.bindingId == expected.bindingId &&
        current.definitionIndex == expected.definitionIndex &&
        current.scopeKind == expected.scopeKind &&
        current.objectId == expected.objectId &&
        current.busIndex == expected.busIndex &&
        current.targetGeneration == expected.targetGeneration &&
        std::bit_cast<std::uint32_t>(current.lastContribution) ==
            std::bit_cast<std::uint32_t>(expected.lastContribution) &&
        current.lastHadValue == expected.lastHadValue &&
        current.meterFrozen == expected.meterFrozen &&
        current.ownerCookie == expected.ownerCookie;
}

void BindingTable::CancelPreparedAttachment(
    PreparedAttachmentToken* token) noexcept {
    if (token != nullptr) {
        *token = PreparedAttachmentToken{};
    }
}

void BindingTable::CommitPreparedAttachment(
    PreparedAttachmentToken* token, BusTargetStore* store) noexcept {
    if (token == nullptr || !token->IsPrepared()) {
        return;
    }
    Attachment& slot = m_attachments[token->m_slotIndex];
    if (token->m_operation == PreparedAttachmentToken::Operation::Add) {
        slot = token->m_value;
        ++m_attachmentCount;
        if (m_attachmentCount > m_attachmentHighWater) {
            m_attachmentHighWater = m_attachmentCount;
        }
    } else if (token->m_operation ==
               PreparedAttachmentToken::Operation::Remove) {
        const Attachment& removed = token->m_value;
        if (store != nullptr && removed.scopeKind == kTargetScopeBus &&
            removed.lastHadValue) {
            const Definition& def =
                m_definitions[removed.definitionIndex];
            store->MarkVariableChanged(removed.busIndex, def.targetId,
                                       def.transitionMs);
        }
        slot = Attachment{};
        --m_attachmentCount;
    }
    *token = PreparedAttachmentToken{};
}

const BindingTable::Definition* BindingTable::Find(BindingId id) const {
    for (const Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::Variable &&
            def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

BindingTable::Definition* BindingTable::FindMutable(BindingId id) {
    for (Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::Variable &&
            def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

VnResult BindingTable::RegisterProbeBinding(
    BindingId id, VnObjectId probeStableId,
    std::uint32_t probeGeneration, std::uint32_t scopeKind,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs) {
    if (FindProbeDefinition(id) != nullptr) {
        return VN_IN_USE;
    }
    Curve curve;
    if (!curve.SetPoints(points, handles, pointCount)) {
        return VN_INVALID_ARG;
    }
    if (m_definitionCount >= m_definitions.size()) {
        return kResultNoBinding;
    }
    for (Definition& def : m_definitions) {
        if (def.used) {
            continue;
        }
        def = Definition{};
        def.used = true;
        def.id = id;
        def.sourceKind = BindingSourceKind::MeterProbe;
        def.probeStableId = probeStableId;
        def.probeGeneration = probeGeneration;
        def.scopeKind = scopeKind;
        def.targetId = targetId;
        def.curve = curve;
        def.transitionMs = transitionMs;
        ++m_definitionCount;
        if (m_definitionCount > m_definitionHighWater) {
            m_definitionHighWater = m_definitionCount;
        }
        return VN_OK;
    }
    return kResultNoBinding;
}

VnResult BindingTable::AttachProbe(BindingId id, std::uint32_t scopeKind,
                                   VnObjectId objectId, VnVoiceHandle instanceHandle,
                                   std::uint32_t busIndex,
                                   std::uint32_t targetGeneration) {
    const Definition* def = FindProbeDefinition(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (m_attachmentCount >= m_attachments.size()) {
        return kResultNoBinding;
    }
    for (Attachment& a : m_attachments) {
        if (a.used) {
            continue;
        }
        a = Attachment{};
        a.used = true;
        a.bindingId = id;
        a.definitionIndex =
            static_cast<std::uint32_t>(def - m_definitions.data());
        a.scopeKind = scopeKind;
        a.objectId = objectId;
        a.instanceHandle = instanceHandle;
        a.busIndex = busIndex;
        a.targetGeneration = targetGeneration;
        ++m_attachmentCount;
        if (m_attachmentCount > m_attachmentHighWater) {
            m_attachmentHighWater = m_attachmentCount;
        }
        return VN_OK;
    }
    return kResultNoBinding;
}

BindingTable::Definition* BindingTable::FindProbeDefinition(
    BindingId id) noexcept {
    for (Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::MeterProbe &&
            def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

const BindingTable::Definition* BindingTable::FindProbeDefinition(
    BindingId id) const noexcept {
    for (const Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::MeterProbe &&
            def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

BindingTable::Attachment* BindingTable::FindProbeAttachment(
    BindingId id, std::uint32_t scopeKind, VnObjectId objectId, VnVoiceHandle instanceHandle,
    std::uint32_t targetGeneration) noexcept {
    for (Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id && a.scopeKind == scopeKind &&
            a.objectId == objectId && a.instanceHandle == instanceHandle &&
            a.targetGeneration == targetGeneration &&
            m_definitions[a.definitionIndex].sourceKind ==
                BindingSourceKind::MeterProbe) {
            return &a;
        }
    }
    return nullptr;
}

void BindingTable::ReleaseProbeAttachment(Attachment& row) noexcept {
    if (row.used) {
        row = Attachment{};
        --m_attachmentCount;
    }
}

void BindingTable::ReleaseProbeDefinition(Definition& row) noexcept {
    if (row.used && row.sourceKind == BindingSourceKind::MeterProbe) {
        row = Definition{};
        --m_definitionCount;
    }
}

bool BindingTable::AnyProbeAttachmentOf(BindingId id) const noexcept {
    for (const Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id &&
            m_definitions[a.definitionIndex].sourceKind ==
                BindingSourceKind::MeterProbe) {
            return true;
        }
    }
    return false;
}

std::uint32_t BindingTable::ProbeDefinitionCount() const noexcept {
    std::uint32_t count = 0;
    for (const Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::MeterProbe) {
            ++count;
        }
    }
    return count;
}

std::uint32_t BindingTable::ProbeAttachmentCount() const noexcept {
    std::uint32_t count = 0;
    for (const Attachment& a : m_attachments) {
        if (a.used && m_definitions[a.definitionIndex].sourceKind ==
                          BindingSourceKind::MeterProbe) {
            ++count;
        }
    }
    return count;
}

const BindingTable::Attachment*
BindingTable::FindAttachment(BindingId id, std::uint32_t scopeKind,
                             VnObjectId objectId) const {
    for (const Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id && a.scopeKind == scopeKind &&
            a.objectId == objectId) {
            return &a;
        }
    }
    return nullptr;
}

BindingDefinitionView BindingTable::ViewOf(const Definition& def) const {
    BindingDefinitionView view;
    view.definitionId = def.id;
    view.variableId = def.variableId;
    view.targetId = def.targetId;
    view.curve = &def.curve;
    view.transitionMs = &def.transitionMs;
    view.revision = &def.revision;
    return view;
}

bool BindingTable::AnyAttachmentOf(BindingId id) const {
    for (const Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id) {
            return true;
        }
    }
    return false;
}

void BindingTable::EvaluateBusAttachments(const ParamSystem& params,
                                          BusTargetStore& store) {
    // Rebuild from scratch: reset the scratch accumulators, fold every
    // attachment's contribution in-domain, then write the store's
    // Variable slots. A (bus, target) no attachment feeds ends the tick
    // with its slot cleared — a bus with no attachments never sees a
    // slot write. A prospective full reduction that is not finite is
    // refused with zero slot, dirty, or last-contribution mutation.
    for (std::size_t i = 0; i < m_busAccum.size(); ++i) {
        m_busAccum[i] = 0.0f;
        m_busAccumActive[i] = false;
        m_busAccumAccepted[i] = false;
    }
    for (Attachment& a : m_attachments) {
        if (!a.used || a.scopeKind != kTargetScopeBus) {
            continue;
        }
        const Definition& def = m_definitions[a.definitionIndex];
        if (def.sourceKind != BindingSourceKind::Variable) {
            continue; // probe rows fold in the probe sweep, not here
        }
        // Global scope only: a bus has no emitter context (section 5.3).
        // A frozen attachment (section 11.5 loop degradation) contributes
        // neutral — evaluated as if its Variable were unset, so the
        // freeze/unfreeze flip rides the ordinary change detection below.
        float input = 0.0f;
        const bool hasValue =
            !a.meterFrozen &&
            params.ScopedVariableValue(nullptr, 0, def.variableId, &input);
        if (!hasValue) {
            continue;
        }
        const float contribution = def.Evaluate(input);
        const std::size_t index =
            static_cast<std::size_t>(a.busIndex) * kBusTargetCount +
            def.targetId;
        if (index >= m_busAccum.size()) {
            continue; // defensive: attach validated the bus index
        }
        if (!m_busAccumActive[index]) {
            m_busAccum[index] = contribution;
            m_busAccumActive[index] = true;
            continue;
        }
        const TargetMeta* meta =
            FindTargetMeta(kTargetScopeBus, def.targetId);
        m_busAccum[index] = CombineValue(
            meta != nullptr ? meta->domain : CombineDomain::AddDb,
            m_busAccum[index], contribution);
    }
    const std::uint32_t buses = store.MaxBuses();
    for (std::uint32_t bus = 0; bus < buses; ++bus) {
        for (std::uint32_t t = 0; t < kBusTargetCount; ++t) {
            const std::size_t index =
                static_cast<std::size_t>(bus) * kBusTargetCount + t;
            if (index >= m_busAccum.size()) {
                break;
            }
            if (m_busAccumActive[index]) {
                const float prospective = store.ReduceWithSlot(
                    bus, t, BusTargetStore::kSlotVariableIndex,
                    m_busAccum[index]);
                if (!std::isfinite(prospective)) {
                    continue;
                }
                store.SetSlot(bus, t, BusTargetStore::kSlotVariableIndex,
                              m_busAccum[index]);
                m_busAccumAccepted[index] = true;
            } else if (store.SlotActive(
                           bus, t, BusTargetStore::kSlotVariableIndex)) {
                store.ClearSlot(bus, t, BusTargetStore::kSlotVariableIndex);
                m_busAccumAccepted[index] = true;
            }
        }
    }
    for (Attachment& a : m_attachments) {
        if (!a.used || a.scopeKind != kTargetScopeBus) {
            continue;
        }
        const Definition& def = m_definitions[a.definitionIndex];
        if (def.sourceKind != BindingSourceKind::Variable) {
            continue;
        }
        const std::size_t index =
            static_cast<std::size_t>(a.busIndex) * kBusTargetCount +
            def.targetId;
        if (index >= m_busAccumAccepted.size() || !m_busAccumAccepted[index]) {
            continue;
        }
        float input = 0.0f;
        const bool hasValue =
            !a.meterFrozen &&
            params.ScopedVariableValue(nullptr, 0, def.variableId, &input);
        const float contribution = hasValue ? def.Evaluate(input) : 0.0f;
        // Section-9.2 fold: this attachment's transitionMs joins the
        // target's pending ramp aggregate only when its CONTRIBUTION
        // changed — appeared, disappeared, or moved in output. The mark
        // also sets the target dirty, which is what makes the bus flush
        // visit it (two same-tick changes cancelling to an equal reduction
        // publish nothing there — the changed-only reference decides).
        const bool changed = (hasValue != a.lastHadValue) ||
                             (hasValue && contribution != a.lastContribution);
        if (changed) {
            store.MarkVariableChanged(a.busIndex, def.targetId,
                                      def.transitionMs);
        }
        a.lastHadValue = hasValue;
        a.lastContribution = hasValue ? contribution : 0.0f;
    }
    // This pass observed current inputs: every pending-input claim is now
    // realized (as a dirty mark above) or disproved.
    store.ConsumePendingInput();
}

void BindingTable::NoteBusVariableWrite(VnVariableId variableId,
                                        BusTargetStore& store) noexcept {
    for (const Attachment& a : m_attachments) {
        if (!a.used || a.scopeKind != kTargetScopeBus) {
            continue;
        }
        const Definition& def = m_definitions[a.definitionIndex];
        if (def.sourceKind != BindingSourceKind::Variable ||
            def.variableId != variableId) {
            continue;
        }
        // Over-marking is deliberate (a flat curve span may absorb the
        // write): evaluation realizes real changes and retires the rest.
        // Frozen attachments still mark — unfreezing this tick would
        // realize the demand, and under-reserving is the real defect.
        store.MarkPendingInput(a.busIndex, def.targetId);
    }
}

VnResult BindingTable::CanApplyVoiceEffectInstance(
    BindingId id, VnVoiceHandle publicHandle,
    VnVoiceHandle renderHandle) const {
    const Definition* def = Find(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.bindingId == id &&
            (inst.publicHandle == publicHandle ||
             inst.renderHandle == renderHandle)) {
            return VN_IN_USE;
        }
    }
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (!inst.used) {
            return VN_OK;
        }
    }
    return kResultNoBinding;
}

VnResult BindingTable::ApplyVoiceEffectInstance(BindingId id,
                                                VnVoiceHandle publicHandle,
                                                VnVoiceHandle renderHandle,
                                                LogRing* log) {
    const VnResult admitted =
        CanApplyVoiceEffectInstance(id, publicHandle, renderHandle);
    if (admitted != VN_OK) {
        if (admitted == kResultNoBinding) {
            LogPoolExhausted(log, kBindingPoolVoiceInstances,
                             m_voiceEffectInstances.size());
        }
        return admitted;
    }
    const Definition* def = Find(id);
    const std::uint32_t defIndex =
        static_cast<std::uint32_t>(def - m_definitions.data());
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (!inst.used) {
            inst.used = true;
            inst.parked = false;
            inst.pending = false;
            inst.bindingId = id;
            inst.definitionIndex = defIndex;
            inst.publicHandle = publicHandle;
            inst.renderHandle = renderHandle;
            ++m_voiceEffectInstanceCount;
            return VN_OK;
        }
    }
    return kResultNoBinding;
}

VnResult BindingTable::ReserveVoiceEffectInstances(
    VnVoiceHandle publicHandle, const BindingId* ids, std::uint32_t count,
    LogRing* log) {
    if (publicHandle == 0) {
        return VN_INVALID_ARG;
    }
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.publicHandle == publicHandle) {
            return VN_IN_USE;
        }
    }
    const VnResult preflight =
        CanReserveVoiceEffectInstances(ids, count, log);
    if (preflight != VN_OK) {
        return preflight;
    }
    std::uint32_t committed = 0;
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used || committed == count) {
            continue;
        }
        const Definition* def = Find(ids[committed]);
        inst.used = true;
        inst.parked = false;
        inst.pending = true;
        inst.bindingId = ids[committed];
        inst.definitionIndex =
            static_cast<std::uint32_t>(def - m_definitions.data());
        inst.publicHandle = publicHandle;
        inst.renderHandle = 0;
        ++committed;
        ++m_voiceEffectInstanceCount;
    }
    return VN_OK;
}

VnResult BindingTable::CanReserveVoiceEffectInstances(
    const BindingId* ids, std::uint32_t count, LogRing* log) const {
    if (count > 0 && ids == nullptr) {
        return VN_INVALID_ARG;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const Definition* def = Find(ids[i]);
        if (def == nullptr) {
            return VN_UNKNOWN_ID;
        }
        if (def->scopeKind != VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) {
            return VN_INVALID_ARG;
        }
        for (std::uint32_t j = 0; j < i; ++j) {
            if (ids[j] == ids[i]) {
                return VN_IN_USE;
            }
        }
    }
    const std::uint32_t freeCount =
        static_cast<std::uint32_t>(m_voiceEffectInstances.size()) -
        m_voiceEffectInstanceCount;
    if (count > freeCount) {
        LogPoolExhausted(log, kBindingPoolVoiceInstances,
                         m_voiceEffectInstances.size());
        return kResultNoBinding;
    }
    return VN_OK;
}

bool BindingTable::ActivateReservedVoiceEffectInstances(
    VnVoiceHandle publicHandle, VnVoiceHandle renderHandle) {
    bool found = false;
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (!inst.used || inst.publicHandle != publicHandle) {
            continue;
        }
        if (inst.pending) {
            inst.pending = false;
            inst.renderHandle = renderHandle;
            found = true;
        } else if (!inst.parked && inst.renderHandle == renderHandle) {
            found = true;
        }
    }
    return found;
}

void BindingTable::CancelReservedVoiceEffectInstances(
    VnVoiceHandle publicHandle) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.pending &&
            inst.publicHandle == publicHandle) {
            inst = VoiceEffectInstance{};
            --m_voiceEffectInstanceCount;
        }
    }
}

bool BindingTable::ClearVoiceEffectInstance(VnVoiceHandle renderHandle,
                                            BindingId id) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && !inst.pending && inst.bindingId == id &&
            inst.renderHandle == renderHandle) {
            inst = VoiceEffectInstance{};
            --m_voiceEffectInstanceCount;
            return true;
        }
    }
    return false;
}

void BindingTable::RemoveVoiceEffectInstances(VnVoiceHandle renderHandle) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && !inst.pending &&
            inst.renderHandle == renderHandle) {
            inst = VoiceEffectInstance{};
            --m_voiceEffectInstanceCount;
        }
    }
}

void BindingTable::ParkVoiceEffectInstances(VnVoiceHandle renderHandle) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && !inst.pending && !inst.parked &&
            inst.renderHandle == renderHandle) {
            inst.parked = true; // keep the slot: the reservation stays booked
        }
    }
}

void BindingTable::ReturnVoiceEffectInstances(VnVoiceHandle oldRender,
                                              VnVoiceHandle newRender) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.parked && inst.renderHandle == oldRender) {
            inst.renderHandle = newRender;
            inst.parked = false;
        }
    }
}

void BindingTable::PurgeVoiceEffectInstancesOf(
    BindingId id) noexcept {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.bindingId == id) {
            inst = VoiceEffectInstance{};
            --m_voiceEffectInstanceCount;
        }
    }
}

bool BindingTable::HasEffectParamBinding(EffectParamTargetStore::Scope scope,
                                         std::uint64_t objectId,
                                         std::uint32_t slot,
                                         std::uint32_t paramIndex,
                                         std::uint32_t effectType) const {
    if (scope == EffectParamTargetStore::Scope::Bus) {
        for (const Attachment& a : m_attachments) {
            if (!a.used || a.scopeKind != VN_TARGET_SCOPE_BUS_EFFECT_PARAM ||
                a.busIndex != objectId ||
                m_definitions[a.definitionIndex].sourceKind !=
                    BindingSourceKind::Variable) {
                continue;
            }
            const EffectParamLocator loc = UnpackEffectParamLocator(
                m_definitions[a.definitionIndex].targetId);
            if (loc.slot == slot && loc.paramIndex == paramIndex &&
                loc.effectType == effectType) {
                return true;
            }
        }
        return false;
    }
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        // A parked instance's render handle is stale and its store records are
        // cleared, so it never matches a live direct-set objectId; skip it.
        if (!inst.used || inst.pending || inst.parked ||
            inst.renderHandle != objectId) {
            continue;
        }
        const EffectParamLocator loc = UnpackEffectParamLocator(
            m_definitions[inst.definitionIndex].targetId);
        if (loc.slot == slot && loc.paramIndex == paramIndex &&
            loc.effectType == effectType) {
            return true;
        }
    }
    return false;
}

void BindingTable::EvaluateEffectParamBindings(const ParamSystem& params,
                                               EffectParamTargetStore& store,
                                               const EmitterTable* emitters) {
    store.BeginContributionSweep();
    // Bus-effect attachments resolve at global scope (a bus has no emitter).
    for (const Attachment& a : m_attachments) {
        if (!a.used || a.scopeKind != VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
            continue;
        }
        const Definition& def = m_definitions[a.definitionIndex];
        if (def.sourceKind != BindingSourceKind::Variable) {
            continue; // probe rows fold in the probe sweep, not here
        }
        float input = 0.0f;
        if (!params.ScopedVariableValue(nullptr, 0, def.variableId, &input)) {
            continue; // unset Variable contributes nothing (returns to base)
        }
        const EffectParamLocator loc = UnpackEffectParamLocator(def.targetId);
        store.AccumulateBinding(EffectParamTargetStore::Scope::Bus, a.busIndex,
                                loc.slot, loc.paramIndex, loc.effectType,
                                def.Evaluate(input));
    }
    // Voice-effect apply instances resolve at the voice's emitter scope, like a
    // built-in voice target binding.
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        // A parked instance holds its slot as a reservation but drives nothing:
        // its store records were cleared at park and its render handle is stale.
        if (!inst.used || inst.pending || inst.parked) {
            continue;
        }
        const Definition& def = m_definitions[inst.definitionIndex];
        float input = 0.0f;
        if (!params.ScopedVariableValue(emitters,
                                        params.EmitterHandleOf(inst.renderHandle),
                                        def.variableId, &input)) {
            continue;
        }
        const EffectParamLocator loc = UnpackEffectParamLocator(def.targetId);
        store.AccumulateBinding(EffectParamTargetStore::Scope::Voice,
                                inst.renderHandle, loc.slot, loc.paramIndex,
                                loc.effectType, def.Evaluate(input));
    }
    store.EndContributionSweep();
}

} // namespace harken
