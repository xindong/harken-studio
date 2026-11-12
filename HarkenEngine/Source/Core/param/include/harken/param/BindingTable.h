// Public binding system, logic-thread side: the definition registry and
// the attachment pool of the
// three-layer split. Definitions are registered, named, immutable-until-
// replaced mapping rows (one Variable, through one curve, to one scoped
// target, with a transition time); attachments activate a definition
// against one object (a playable id for voice-scope targets, a bus for
// bus-scope targets) and carry no curves; instances are the per-voice
// evaluation state and live in ParamSystem (the M1 BindVoiceTarget table
// evolved into the instance layer).
//
// Registration and attachment have NO audible side effect; only instances
// evaluate. Every entry point validates exhaustively before touching pool
// state and refuses with zero side effects. Replace validates fully, then
// stages a pending revision that installs at the next Vn_Update in the
// fixed first phase of the update (section 14.3); every reader — bus
// attachments, live voice instances, parked and pending-start instances —
// evaluates the new revision from that tick on, because instances read
// the definition's storage through live pointers instead of holding curve
// copies.
//
// Fixed capacities (section 16), preallocated at construction, zero
// audio-thread involvement — the audio thread never learns bindings
// exist. Logic thread only.
#ifndef HARKEN_PARAM_BINDING_TABLE_H
#define HARKEN_PARAM_BINDING_TABLE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/LogRing.h"
#include "harken/core/StableRowStorage.h"
#include "harken/param/BusTargetStore.h"
#include "harken/param/Curve.h"
#include "harken/param/EffectParamTargetStore.h"
#include "harken/param/ParamSystem.h"
#include "harken/param/ParamTargetTable.h"

#include <cstdint>
#include <array>
#include <utility>
#include <vector>

namespace harken {

// Internal alias of the public binding id (u64, caller-owned space).
using BindingId = VnBindingId;

// What a binding definition reads as its source: a public Variable, or a
// private meter probe's dBFS level. One shared definition/attachment pool
// serves both kinds; the kind gates which entry points may touch a row and
// which evaluation sweep folds it.
enum class BindingSourceKind : std::uint8_t {
    Variable = 0,
    MeterProbe = 1,
};

// Internal alias of the public VN_NO_BINDING result: a binding pool —
// definitions, attachments, or a voice's instance slots — is exhausted;
// the log record names which.
inline constexpr VnResult kResultNoBinding = VN_NO_BINDING;

enum class BindingDependencyKind : std::uint32_t { Global, Local, Definition, Probe, Target };

struct BindingDependencyIdentity {
    VnObjectId source{};
    std::uint64_t object = 0;
    std::uint32_t scope = 0;
    std::uint32_t target = 0;
    std::uint32_t generation = 0;
    std::uint32_t detail = 0;
    bool operator==(const BindingDependencyIdentity&) const = default;
};
static_assert(sizeof(BindingDependencyIdentity) == 40);

struct BindingDependencyToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
};

struct BindingDirtyTarget {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
};

// Serialized logic-side index. Facts and their original contribution order stay with their owner.
class BindingDependencyIndex {
public:
    BindingDependencyIndex() = default;
    BindingDependencyIndex(const BindingDependencyIndex& other);
    using ReadIdentity = BindingDependencyIdentity (*)(const void*, BindingDependencyKind) noexcept;
    bool Reset(std::uint32_t bindingCapacity, std::uint32_t targetCapacity,
               std::uint32_t keyCapacity = 0) noexcept;
    bool PrepareBinding(std::uint32_t slot) noexcept;
    using BackingPreparation = std::array<StableStoragePreparation, 3>;
    using BackingUse = std::array<StableStoragePreparedUse, 3>;
    bool PrepareBacking(std::uint32_t count, BackingPreparation* out) noexcept;
    static BackingUse UseBacking(const BackingPreparation* backing) noexcept;
    bool HasBindingPreparation(std::uint32_t slot) const noexcept;
    void CancelBindingPreparation(std::uint32_t slot) noexcept;
    bool Register(std::uint32_t slot, const void* fact, ReadIdentity read, std::uint32_t order,
                  bool local, bool liveDefinition, bool probe, BindingDependencyToken* out) noexcept;
    bool Relocate(BindingDependencyToken token, const void* fact, ReadIdentity read,
                  std::uint32_t currentOrder = UINT32_MAX) noexcept;
    bool DetachLocal(BindingDependencyToken token) noexcept;
    bool Remove(BindingDependencyToken token) noexcept;
    void Notify(BindingDependencyKind kind, const BindingDependencyIdentity& identity) noexcept;
    void NotifyVariable(VnVariableId variable, VnEmitterHandle emitter = 0) noexcept;
    void NotifyDefinition(BindingId definition) noexcept;
    void Mark(BindingDependencyToken token) noexcept;
    bool TargetOf(BindingDependencyToken token, BindingDirtyTarget* out) const noexcept;
    std::uint64_t TargetVersion(BindingDirtyTarget target) const noexcept;
    bool DirtyTargetAt(std::uint32_t index, BindingDirtyTarget* out) const noexcept;
    bool TargetIdentity(BindingDirtyTarget target, BindingDependencyIdentity* out) const noexcept;
    BindingDependencyToken FirstContribution(BindingDirtyTarget target) const noexcept;
    BindingDependencyToken NextContribution(BindingDependencyToken token) const noexcept;
    const void* Fact(BindingDependencyToken token) const noexcept;
    bool CommitTarget(BindingDirtyTarget target, std::uint64_t version) noexcept;
    std::size_t StorageBytes() const noexcept;
    std::uint32_t BindingCapacity() const noexcept { return bindingCapacity_; }
    std::uint32_t ParamBindingCapacity() const noexcept { return paramBindingCapacity_; }
    std::uint32_t BackedBindingCapacity() const noexcept { return inputs_.ConstructedCount() / 3u; }

private:
    friend class BindingTable;
    bool PrepareInputs(std::uint32_t count, std::uint32_t occupied,
                       StableStoragePreparation* backing = nullptr) noexcept;
    bool PrepareKeys(std::uint32_t count, std::uint32_t occupied,
                     StableStoragePreparation* backing = nullptr) noexcept;
    bool PrepareTargets(std::uint32_t count, std::uint32_t occupied,
                        StableStoragePreparation* backing = nullptr) noexcept;
    struct InputNode {
        const void* fact = nullptr;
        ReadIdentity read = nullptr;
        std::uint32_t key = UINT32_MAX;
        std::uint32_t nextSource = UINT32_MAX;
        std::uint32_t nextTarget = UINT32_MAX;
        std::uint32_t target = UINT32_MAX;
        std::uint32_t generation = 1;
        std::uint32_t order = 0;
    };
    static_assert(sizeof(InputNode) == 40);
    struct Locator {
        const void* fact = nullptr;
        ReadIdentity read = nullptr;
        std::uint64_t factVersion = 0;
        std::uint32_t representative = UINT32_MAX;
        std::uint32_t targetRow = UINT32_MAX;
        std::uint64_t order = 0;
    };
    struct Key {
        union { Locator locator; BindingDependencyIdentity retired; };
        std::uint32_t head = UINT32_MAX;
        std::uint32_t count = 0;
        std::uint32_t left = UINT32_MAX;
        std::uint32_t right = UINT32_MAX;
        std::uint32_t parent = UINT32_MAX;
        std::uint32_t kindHeight = 1;
        Key() noexcept : locator{} {}
    };
    static_assert(sizeof(Key) == 64);
    struct DirtyRow {
        std::uint64_t generation = 1;
        std::uint64_t version = 0;
        std::uint32_t key = UINT32_MAX;
        std::uint32_t membership = UINT32_MAX;
    };
    static_assert(sizeof(DirtyRow) == 24);
    const InputNode* Resolve(BindingDependencyToken token) const noexcept;
    const DirtyRow* Resolve(BindingDirtyTarget target) const noexcept;
    BindingDependencyIdentity Identity(const Key& key) const noexcept;
    static BindingDependencyKind Kind(const Key& key) noexcept;
    std::uint32_t Find(BindingDependencyKind kind, const BindingDependencyIdentity& identity) const noexcept;
    std::uint32_t EnsureKey(BindingDependencyKind kind, std::uint32_t node) noexcept;
    void LinkSource(std::uint32_t node, std::uint32_t key) noexcept;
    void UnlinkSource(std::uint32_t node) noexcept;
    void MarkTarget(std::uint32_t key) noexcept;
    void Reanchor(Key& key, std::uint32_t node) noexcept;
    void EraseKey(std::uint32_t key) noexcept;
    void Rebalance(std::uint32_t key) noexcept;
    std::uint32_t Rotate(std::uint32_t key, bool left) noexcept;
    std::uint32_t Height(std::uint32_t key) const noexcept;
    void UpdateHeight(std::uint32_t key) noexcept;
    std::uint32_t bindingCapacity_ = 0;
    std::uint32_t paramBindingCapacity_ = 0;
    std::uint32_t targetCapacity_ = 0;
    std::uint32_t keyCapacity_ = 0;
    std::uint32_t root_ = UINT32_MAX;
    std::uint64_t version_ = 0;
    StableRowStorage<InputNode> inputs_;
    StableRowStorage<Key> keys_;
    StableRowStorage<DirtyRow> targets_;
    std::vector<std::uint32_t> freeKeys_, freeTargets_, dirtyTargets_;
    std::vector<std::uint64_t> preparedBindings_;
    std::uint32_t preparedBindingCount_ = 0;
};

class BindingTable {
public:
    // One registered definition. curve and transitionMs are the CURRENT
    // revision's values — instances and bus attachments read them through
    // pointers, so an installed Replace is live everywhere at once.
    // variableId, scopeKind and targetId are the frozen structural
    // identity (section 5.5). The pending block holds a validated
    // replacement awaiting its install tick.
    struct Definition {
        bool used = false;
        BindingSourceKind sourceKind = BindingSourceKind::Variable;
        BindingId id{};
        VnVariableId variableId{};
        // MeterProbe-kind rows: the probe identity the source value reads
        // from (variableId stays 0). Generation-exact — a reborn probe
        // never feeds an old definition.
        VnObjectId probeStableId{};
        std::uint32_t probeGeneration = 0;
        std::uint32_t scopeKind = 0;
        std::uint32_t targetId = 0;
        Curve curve;
        std::uint32_t transitionMs = 0;
        std::uint32_t revision = 1;
        bool pendingInstall = false;
        Curve pendingCurve;
        std::uint32_t pendingTransitionMs = 0;
        // Provenance imprint: 0 for a caller registration, the owning
        // pack's handle for a pack-applied one. A caller who unregisters a
        // pack's definition and registers a same-id one of their own mints
        // a 0-imprinted slot, so the pack's later unwind and diff paths
        // can tell the generations apart where the id alone cannot.
        std::uint64_t ownerCookie = 0;

        float Evaluate(float input) const {
            return curve.Eval(input, BindingCurveDomain(scopeKind, targetId));
        }
    };

    // One attachment: definition x object. busIndex is the dense bus
    // index resolved at attach time for Bus-hosted scopes (ordinary Bus
    // targets and Bus Effect Parameters); unused for Voice scope.
    //
    // BUS attachments additionally carry per-attachment change detection
    // (the instance-layer discipline applied to the bus evaluation):
    // lastContribution is the curve OUTPUT the attachment last folded
    // (section 9.2 folds a binding's transitionMs only when its
    // CONTRIBUTION changed — a flat curve segment moves the input without
    // moving the output and folds nothing), lastHadValue whether it
    // contributed at all. meterFrozen is the section-11.5 post-wiring loop
    // degradation: a frozen attachment contributes neutral (skipped like an
    // unset Variable) until the loop opens, and the presence flip through
    // the ordinary change detection is what ramps the freeze/unfreeze with
    // the definition's transitionMs.
    struct Attachment {
        bool used = false;
        BindingId bindingId{};
        std::uint32_t definitionIndex = 0;
        std::uint32_t scopeKind = 0;
        VnObjectId objectId = {};
        VnVoiceHandle instanceHandle = 0;
        std::uint32_t busIndex = 0;
        // The exact target generation captured at attach time (0 for
        // legacy callers without generation identity). A recycled bus or
        // voice slot never matches an old probe attachment.
        std::uint32_t targetGeneration = 0;
        float lastContribution = 0.0f;
        bool lastHadValue = false;
        bool meterFrozen = false;
        // Provenance imprint: 0 for a caller attach, the owning pack's
        // handle for a pack-applied one. Detach clears the slot whole, so
        // a caller who detaches a pack's triple and re-attaches the SAME
        // triple mints a fresh 0-imprinted attachment — the per-triple
        // existence probe alone cannot distinguish that rebirth from the
        // pack's own, but the imprint can.
        std::uint64_t ownerCookie = 0;
        const Definition* dependencyDefinition = nullptr;
        BindingDependencyToken dependency{};
    };

    struct PreparedDefinitionToken {
        bool IsPrepared() const { return m_operation != Operation::None; }
        std::uint32_t SlotIndex() const { return m_slotIndex; }

    private:
        friend class BindingTable;
        enum class Operation : std::uint8_t {
            None,
            Add,
            Replace,
            Remove,
        };
        Operation m_operation = Operation::None;
        std::uint32_t m_slotIndex = 0;
        Definition m_value;
    };

    struct PreparedAttachmentToken {
        bool IsPrepared() const { return m_operation != Operation::None; }
        std::uint32_t SlotIndex() const { return m_slotIndex; }

    private:
        friend class BindingTable;
        enum class Operation : std::uint8_t {
            None,
            Add,
            Remove,
        };
        Operation m_operation = Operation::None;
        std::uint32_t m_slotIndex = 0;
        Attachment m_value;
    };

    // One live voice-effect-parameter apply instance: a definition (scope
    // VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) applied to one live render voice via
    // Vn_ApplyVoiceBinding. Unlike the built-in voice targets (whose instances
    // live in ParamSystem), effect-parameter voice applies are tracked here and
    // fold into the EffectParamTargetStore, so ParamSystem stays untouched.
    //
    // parked marks an instance whose voice is audibility-parked: it keeps its
    // pool slot (a held reservation, mirroring the section-5.6 parked-instance
    // discipline) so a return can retarget it to the fresh render handle without
    // re-allocating and thus without any chance of refusal, and so a new apply
    // that would exceed capacity while the voice is parked refuses correctly.
    // A parked instance contributes nothing (its store records are cleared at
    // park) and its render handle is stale until the return rewrites it.
    struct VoiceEffectInstance {
        bool used = false;
        bool parked = false;
        bool pending = false;
        BindingId bindingId{};
        std::uint32_t definitionIndex = 0;
        VnVoiceHandle publicHandle = 0;
        VnVoiceHandle renderHandle = 0;
        const Definition* dependencyDefinition = nullptr;
        VnEmitterHandle dependencyEmitter = 0;
        BindingDependencyToken dependency{};
    };

    BindingTable(std::uint32_t maxDefinitions, std::uint32_t maxAttachments,
                 std::uint32_t maxBuses, std::uint32_t renderBindingCapacity = 0,
                 std::uint32_t targetCapacity = 0);
    BindingTable(const BindingTable& other);
    void AttachVoiceCatalogStorage(void* context, bool (*prepare)(void*) noexcept) noexcept {
        m_voiceCatalogContext = context;
        m_prepareVoiceCatalog = prepare;
    }
    BindingDependencyIndex& Dependencies() noexcept { return m_dependencies; }
    const BindingDependencyIndex& Dependencies() const noexcept { return m_dependencies; }

    // Registers a definition. Validates exhaustively before touching pool
    // state: the curve through the Curve::SetPoints rules (finiteness,
    // strictly increasing x, valid Bezier handles), the scoped target against the
    // metadata table, and the section-9.1 transition rule (a nonzero
    // transitionMs on a target whose metadata says supportsTransition ==
    // false refuses VN_INVALID_ARG — snap-only targets refuse loudly,
    // never silently ignore). A registered id refuses VN_IN_USE; a full
    // pool refuses kResultNoBinding with a log record naming the pool.
    // `ownerCookie` stamps the definition's provenance (0 = caller, a
    // pack handle for pack-applied entries); it is identity metadata
    // only and takes no part in validation.
    VnResult RegisterBinding(BindingId id, VnVariableId variableId,
                             std::uint32_t scopeKind, std::uint32_t targetId,
                             const CurvePoint* points,
                             const CurveHandlePair* handles,
                             std::uint32_t pointCount,
                             std::uint32_t transitionMs, LogRing* log,
                             std::uint64_t ownerCookie = 0);

    // Pure registration admission (the CanAttach precedent): the exact
    // refusal RegisterBinding would produce — the same curve rules, scoped
    // target metadata and section-9.1 transition legality, VN_IN_USE for a
    // registered id, kResultNoBinding for a full definition pool — with
    // zero side effects and NO log record; the mutating registration keeps
    // sole ownership of the pool-naming record and its refusal counter.
    // The registration path runs this admission first, so the two cannot
    // drift.
    VnResult CanRegister(BindingId id, VnVariableId variableId,
                         std::uint32_t scopeKind, std::uint32_t targetId,
                         const CurvePoint* points, const CurveHandlePair* handles,
                         std::uint32_t pointCount,
                         std::uint32_t transitionMs) const;

    // Removes a definition. The table refuses VN_IN_USE while any
    // attachment references it; the caller (the engine seam) extends the
    // section-5.4 rule to live voice instances and to parked and
    // pending-start identity holders before calling here. Detach first —
    // the honest static rule, no hidden cascade.
    VnResult UnregisterBinding(BindingId id);

    // Live in-place replace (section 5.5). Structural identity is frozen:
    // variableId, scopeKind and targetId must equal the registered
    // definition's, else VN_INVALID_ARG — only the curve and transitionMs
    // may change (re-pointing a binding is detach + unregister + register
    // by design). The replacement validates fully (same rules as
    // registration), then stages as a pending revision; nothing the
    // engine evaluates changes until the install phase at the next
    // Vn_Update. Same-tick multiple replaces stage last-wins.
    VnResult ReplaceBinding(BindingId id, VnVariableId variableId,
                            std::uint32_t scopeKind, std::uint32_t targetId,
                            const CurvePoint* points,
                            const CurveHandlePair* handles,
                            std::uint32_t pointCount,
                            std::uint32_t transitionMs);

    VnResult PrepareRegisterDefinition(
        BindingId id, VnVariableId variableId, std::uint32_t scopeKind,
        std::uint32_t targetId, const CurvePoint* points,
        const CurveHandlePair* handles, std::uint32_t pointCount,
        std::uint32_t transitionMs, std::uint32_t slotIndex,
        std::uint64_t ownerCookie, PreparedDefinitionToken* out,
        bool slotWillBeVacant = false,
        bool idWillBeRemoved = false);
    VnResult PrepareReplaceDefinition(
        BindingId id, VnVariableId variableId, std::uint32_t scopeKind,
        std::uint32_t targetId, const CurvePoint* points,
        const CurveHandlePair* handles, std::uint32_t pointCount,
        std::uint32_t transitionMs, PreparedDefinitionToken* out);
    VnResult PrepareRemoveDefinition(
        BindingId id, PreparedDefinitionToken* out,
        bool attachmentsWillBeDetached = false);
    void CancelPreparedDefinition(PreparedDefinitionToken* token) noexcept;
    void CommitPreparedDefinition(PreparedDefinitionToken* token,
                                  bool installBusRevisionImmediately = false) noexcept;

    // The fixed first phase of Vn_Update (section 14.2/14.3): installs
    // every pending revision into the definition's live storage and bumps
    // its revision counter, so every reader evaluates the new curve from
    // this tick on. `onInstalled(BindingId)` runs per installed
    // definition — the engine uses it to mark instance demand.
    template <typename CanInstall, typename OnInstalled>
    void InstallPendingRevisions(CanInstall&& canInstall,
                                 OnInstalled&& onInstalled) {
        if (m_definitionCount == 0) return;
        for (Definition& def : m_definitions) {
            if (!def.used || !def.pendingInstall || !canInstall(def)) {
                continue;
            }
            def.curve = def.pendingCurve;
            def.transitionMs = def.pendingTransitionMs;
            def.pendingInstall = false;
            ++def.revision;
            m_dependencies.NotifyDefinition(def.id);
            onInstalled(def.id);
        }
    }

    template <typename Fn>
    void InstallPendingRevisions(Fn&& onInstalled) {
        InstallPendingRevisions([](const Definition&) { return true; },
                                std::forward<Fn>(onInstalled));
    }

    // Pure attach admission: the exact refusal Attach() would produce,
    // with zero side effects. The engine seam runs it before growing the
    // section-10.4 worst-case bound, so a pool refusal never needs a
    // capacity undo. VOICE attachments pass the playable id; BUS
    // attachments pass the bus id plus its resolved dense index.
    VnResult CanAttach(BindingId id, std::uint32_t scopeKind,
                       VnObjectId objectId) const;

    // Activates a definition against one object. Same validation as
    // CanAttach (scope must equal the definition's targetScopeKind,
    // duplicate definition x object refuses VN_IN_USE, a full pool
    // refuses kResultNoBinding with the pool named in the log record).
    // `ownerCookie` stamps the attachment's provenance (0 = caller, a
    // pack handle for pack-applied triples); identity metadata only.
    VnResult Attach(BindingId id, std::uint32_t scopeKind,
                    VnObjectId objectId, std::uint32_t busIndex,
                    LogRing* log, std::uint64_t ownerCookie = 0,
                    std::uint32_t targetGeneration = 0);

    // Deactivates one attachment. VN_UNKNOWN_ID when no such attachment
    // exists. The engine seam neutralizes the live instances born from a
    // VOICE attachment (section 5.3) around this call. For a BUS
    // attachment that was contributing, `store` (when non-null) receives
    // the departure: the target marks dirty with the definition's
    // transitionMs folded, so the contribution ramps back to neutral over
    // it at the next publication (section 5.3's detach ramp).
    VnResult Detach(BindingId id, std::uint32_t scopeKind,
                    VnObjectId objectId, BusTargetStore* store = nullptr);

    VnResult PrepareAttach(BindingId id, std::uint32_t scopeKind,
                           VnObjectId objectId, std::uint32_t busIndex,
                           std::uint32_t slotIndex,
                           std::uint64_t ownerCookie,
                           PreparedAttachmentToken* out,
                           std::uint32_t preparedDefinitionIndex =
                               0xFFFFFFFFu,
                           bool definitionWillBeAdded = false,
                           bool slotWillBeVacant = false,
                           bool duplicateWillBeDetached = false,
                           std::uint32_t targetGeneration = 0);
    VnResult PrepareDetach(BindingId id, std::uint32_t scopeKind,
                           VnObjectId objectId,
                           PreparedAttachmentToken* out);
    bool CanCommitPreparedAttachment(const PreparedAttachmentToken& token) const noexcept;
    void CancelPreparedAttachment(PreparedAttachmentToken* token) noexcept;
    void CommitPreparedAttachment(PreparedAttachmentToken* token,
                                  BusTargetStore* store) noexcept;

    // --- private meter-probe rows (BindingSourceKind::MeterProbe) ---
    // The probe seam (MeterProbeSystem) owns these entries. The shared
    // pools carry the rows under the shared maxBindings /
    // maxBindingAttachments capacities; the public Variable entries above
    // never see probe rows (Find and every Variable sweep are kind-gated),
    // and these entries never see Variable rows.

    VnResult RegisterProbeBinding(BindingId id, VnObjectId probeStableId,
                                  std::uint32_t probeGeneration,
                                  std::uint32_t scopeKind,
                                  std::uint32_t targetId,
                                  const CurvePoint* points,
                                  const CurveHandlePair* handles,
                                  std::uint32_t pointCount,
                                  std::uint32_t transitionMs);
    VnResult AttachProbe(BindingId id, std::uint32_t scopeKind,
                         VnObjectId objectId, VnVoiceHandle instanceHandle,
                         std::uint32_t busIndex, std::uint32_t targetGeneration);

    Definition* FindProbeDefinition(BindingId id) noexcept;
    const Definition* FindProbeDefinition(BindingId id) const noexcept;
    Attachment* FindProbeAttachment(BindingId id, std::uint32_t scopeKind,
                                    VnObjectId objectId, VnVoiceHandle instanceHandle,
                                    std::uint32_t targetGeneration) noexcept;
    const Definition& DefinitionOf(const Attachment& a) const {
        return m_definitions[a.definitionIndex];
    }
    void ReleaseProbeAttachment(Attachment& row) noexcept;
    void ReleaseProbeDefinition(Definition& row) noexcept;
    bool AnyProbeAttachmentOf(BindingId id) const noexcept;

    std::uint32_t ProbeDefinitionCount() const noexcept;
    std::uint32_t ProbeAttachmentCount() const noexcept;

    template <typename Fn>
    void ForEachProbeDefinition(Fn&& fn) const {
        for (const Definition& def : m_definitions) {
            if (def.used &&
                def.sourceKind == BindingSourceKind::MeterProbe) {
                fn(def);
            }
        }
    }

    template <typename Fn>
    void ForEachProbeAttachment(Fn&& fn) const {
        for (const Attachment& a : m_attachments) {
            if (a.used && m_definitions[a.definitionIndex].sourceKind ==
                              BindingSourceKind::MeterProbe) {
                fn(a, m_definitions[a.definitionIndex]);
            }
        }
    }

    // Mutable twin for the probe evaluation and withdrawal sweeps. fn may
    // release the row through ReleaseProbeAttachment.
    template <typename Fn>
    void ForEachProbeAttachmentMutable(Fn&& fn) {
        for (Attachment& a : m_attachments) {
            if (a.used && m_definitions[a.definitionIndex].sourceKind ==
                              BindingSourceKind::MeterProbe) {
                fn(a, m_definitions[a.definitionIndex]);
            }
        }
    }

    template <typename Fn>
    void ForEachProbeDefinitionMutable(Fn&& fn) {
        for (Definition& def : m_definitions) {
            if (def.used &&
                def.sourceKind == BindingSourceKind::MeterProbe) {
                fn(def);
            }
        }
    }

    // Definition lookup (nullptr when the id names none — probe rows are
    // invisible here; the probe entries above own that namespace). The
    // pointer is
    // stable for the pool's lifetime but the slot recycles after a
    // successful unregister — callers do not hold it across mutations.
    const Definition* Find(BindingId id) const;

    // The live views an instance evaluates through (section 5.5: pointers
    // into this pool's fixed storage, so an installed Replace is read by
    // every instance without any per-instance update).
    BindingDefinitionView ViewOf(const Definition& def) const;

    // Every VOICE-scope attachment whose object is the played id or the
    // resolved leaf id — the activation rule of section 5.3 (intermediate
    // containers consulted mid-resolution never match). fn(const
    // Definition&, VnObjectId attachmentObjectId).
    template <typename Fn>
    void ForEachVoiceAttachment(VnObjectId playedId, VnObjectId leafId,
                                Fn&& fn) const {
        for (const Attachment& a : m_attachments) {
            if (!a.used || a.scopeKind != kTargetScopeVoice ||
                m_definitions[a.definitionIndex].sourceKind !=
                    BindingSourceKind::Variable) {
                continue;
            }
            if (a.objectId != playedId && a.objectId != leafId) {
                continue;
            }
            fn(m_definitions[a.definitionIndex], a.objectId);
        }
    }

    // Whether any attachment references a definition (part of the
    // section-5.4 unregister scan).
    bool AnyAttachmentOf(BindingId id) const;

    // Live attachments referencing a definition, any scope. The pack
    // unload gate compares it against the pack's own attachment count to
    // detect foreign references without mutating anything.
    std::uint32_t AttachmentCountOf(BindingId id) const {
        std::uint32_t count = 0;
        for (const Attachment& a : m_attachments) {
            if (a.used && a.bindingId == id) {
                ++count;
            }
        }
        return count;
    }

    // Whether one exact (definition, scope, object) attachment is live.
    // The pack ledger verifies its recorded triples with this instead of
    // count arithmetic: a caller may detach a pack's attachment and
    // attach a different one to the same definition, and only per-triple
    // existence tells the two apart.
    bool HasAttachment(BindingId id, std::uint32_t scopeKind,
                       VnObjectId objectId) const {
        for (const Attachment& a : m_attachments) {
            if (a.used && a.bindingId == id && a.scopeKind == scopeKind &&
                a.objectId == objectId) {
                return true;
            }
        }
        return false;
    }

    // Whether one exact triple is live AND still carries the given
    // provenance imprint. This is the generation-safe form of the probe
    // above: a caller may detach a pack's triple and re-attach the SAME
    // triple, and only the imprint tells the reborn caller attachment
    // apart from the pack's original — existence (and any count) cannot.
    bool HasOwnedAttachment(BindingId id, std::uint32_t scopeKind,
                            VnObjectId objectId,
                            std::uint64_t ownerCookie) const {
        for (const Attachment& a : m_attachments) {
            if (a.used && a.bindingId == id && a.scopeKind == scopeKind &&
                a.objectId == objectId) {
                return a.ownerCookie == ownerCookie;
            }
        }
        return false;
    }

    // The provenance imprint of a registered definition, or 0 when the id
    // names none (0 is also the caller imprint — the pack paths compare
    // against their own nonzero handle, so absence and caller ownership
    // both read as "not this pack's").
    std::uint64_t DefinitionOwnerOf(BindingId id) const {
        const Definition* def = Find(id);
        return def != nullptr ? def->ownerCookie : 0;
    }

    // --- effect-parameter binding scopes (VN_TARGET_SCOPE_*_EFFECT_PARAM) ---

    // Applies a VOICE_EFFECT_PARAM definition to one live render voice. The
    // definition's targetId is the packed locator (effect type / slot / param).
    // Refuses VN_IN_USE for a duplicate (definition, voice) and kResultNoBinding
    // when the instance pool is full; zero side effects on refusal. The caller
    // (engine seam) validates the definition scope and the live handle first.
    VnResult ApplyVoiceEffectInstance(BindingId id,
                                      VnVoiceHandle publicHandle,
                                      VnVoiceHandle renderHandle,
                                      LogRing* log,
                                      VnEmitterHandle emitter = 0);
    VnResult CanApplyVoiceEffectInstance(BindingId id,
                                         VnVoiceHandle publicHandle,
                                         VnVoiceHandle renderHandle) const;
    VnResult ApplyVoiceEffectInstance(BindingId id,
                                      VnVoiceHandle renderHandle,
                                      LogRing* log) {
        return ApplyVoiceEffectInstance(id, renderHandle, renderHandle, log);
    }

    VnResult ReserveVoiceEffectInstances(VnVoiceHandle publicHandle,
                                         const BindingId* ids,
                                         std::uint32_t count, LogRing* log);
    VnResult CanReserveVoiceEffectInstances(const BindingId* ids,
                                            std::uint32_t count,
                                            LogRing* log) const;
    // Activates a pending batch, or confirms an already-retargeted live batch
    // for the same public/render identity during audibility return.
    bool ActivateReservedVoiceEffectInstances(VnVoiceHandle publicHandle,
                                              VnVoiceHandle renderHandle,
                                              VnEmitterHandle emitter = 0);
    void CancelReservedVoiceEffectInstances(VnVoiceHandle publicHandle);

    // Removes one voice's apply instance of a definition. False when none.
    bool ClearVoiceEffectInstance(VnVoiceHandle renderHandle, BindingId id);

    // Removes every effect-parameter apply instance of one render voice, live or
    // parked (voice-end reclamation, called beside the store's ClearObject). A
    // parked voice that ends without returning is reclaimed by its stale render
    // handle here, releasing the held reservation.
    void RemoveVoiceEffectInstances(VnVoiceHandle renderHandle);

    // Audibility park: marks every live apply instance of one render voice as
    // parked. The instance KEEPS its pool slot (a held reservation), so its
    // capacity stays booked while the voice is parked and the return cannot be
    // refused. No-op for an already-parked handle.
    void ParkVoiceEffectInstances(VnVoiceHandle renderHandle);
    bool PrepareVoiceEffectInstancePark(VnVoiceHandle renderHandle) noexcept;
    void CancelVoiceEffectInstancePark(VnVoiceHandle renderHandle) noexcept;

    // Audibility return: retargets every parked apply instance of oldRender to
    // newRender and clears the parked mark. A pure field rewrite over already-
    // held slots — never allocates, so it cannot fail (all-or-nothing by
    // construction).
    void ReturnVoiceEffectInstances(VnVoiceHandle oldRender,
                                    VnVoiceHandle newRender,
                                    VnEmitterHandle emitter = 0);

    // Voice-effect apply instances of a definition, live or parked, for the
    // section-5.4 unregister reference scan and the pack-unload gate. Because a
    // parked instance keeps its slot, the parked case is caught by this one scan
    // (no separate parked-id list): the callee is told whether each instance is
    // parked so it can treat a parked one as an unconditional reference (its
    // stale render handle no longer resolves) and a live one as a reference only
    // while its render voice resolves. fn(VnVoiceHandle renderHandle, bool
    // parked).
    template <typename Fn>
    void ForEachVoiceEffectInstanceOf(BindingId id, Fn&& fn) const {
        for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
            if (inst.used && inst.bindingId == id) {
                fn(inst.renderHandle, inst.parked || inst.pending);
            }
        }
    }

    template <typename Fn>
    void ForEachVoiceEffectInstance(Fn&& fn) const {
        for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
            if (!inst.used || inst.pending || inst.parked) {
                continue;
            }
            fn(inst, m_definitions[inst.definitionIndex]);
        }
    }

    template <typename Fn>
    void ForEachVoiceEffectInstanceForHandle(VnVoiceHandle publicHandle, Fn&& fn) const {
        for (const auto& inst : m_voiceEffectInstances)
            if (inst.used && inst.publicHandle == publicHandle)
                fn(inst, m_definitions[inst.definitionIndex]);
    }

    template <typename Fn>
    void ForEachVoiceEffectVariableDependency(VnVoiceHandle publicHandle,
                                              Fn&& fn) const {
        for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
            if (!inst.used || inst.publicHandle != publicHandle) {
                continue;
            }
            const Definition& definition =
                m_definitions[inst.definitionIndex];
            if (definition.sourceKind == BindingSourceKind::Variable) {
                fn(definition.variableId);
            }
        }
    }

    template <typename Fn>
    void ForEachEffectInstallVariableDependency(
        EffectParamTargetStore::Scope scope, std::uint64_t objectId,
        std::uint32_t slot, std::uint32_t effectType, Fn&& fn) const {
        if (scope == EffectParamTargetStore::Scope::Bus) {
            for (const Attachment& attachment : m_attachments) {
                if (!attachment.used ||
                    attachment.scopeKind !=
                        VN_TARGET_SCOPE_BUS_EFFECT_PARAM ||
                    attachment.busIndex != objectId) {
                    continue;
                }
                const Definition& definition =
                    m_definitions[attachment.definitionIndex];
                const EffectParamLocator locator =
                    UnpackEffectParamLocator(definition.targetId);
                if (definition.sourceKind == BindingSourceKind::Variable &&
                    locator.slot == slot && locator.effectType == effectType) {
                    fn(definition.variableId);
                }
            }
            return;
        }
        for (const VoiceEffectInstance& instance : m_voiceEffectInstances) {
            if (!instance.used || instance.pending || instance.parked ||
                instance.renderHandle != objectId) {
                continue;
            }
            const Definition& definition =
                m_definitions[instance.definitionIndex];
            const EffectParamLocator locator =
                UnpackEffectParamLocator(definition.targetId);
            if (definition.sourceKind == BindingSourceKind::Variable &&
                locator.slot == slot && locator.effectType == effectType) {
                fn(definition.variableId);
            }
        }
    }

    // Removes every voice-effect apply instance of a definition, live or inert
    // (called after a successful unregister the reference scan proved safe),
    // mirroring PurgeInstancesOf — so no instance can read a recycled
    // definition slot and drive the wrong definition.
    void PurgeVoiceEffectInstancesOf(BindingId id) noexcept;

    // Whether any effect-parameter binding (bus attachment or voice apply
    // instance) currently drives (scope, objectId, slot, paramIndex) AND whose
    // locator expects `effectType`. The direct-set interpose consults this with
    // the currently-installed type: a stale binding left over an in-place type
    // replace does not match, so its direct set takes the unbound (immediate
    // emit) path, byte-identical to the pre-binding behavior.
    bool HasEffectParamBinding(EffectParamTargetStore::Scope scope,
                               std::uint64_t objectId, std::uint32_t slot,
                               std::uint32_t paramIndex,
                               std::uint32_t effectType) const;

    // One contribution sweep over every effect-parameter binding into the
    // store: bus-effect attachments (scope 3) and voice-effect apply instances
    // (scope 2) resolve their Variable through a curve and fold into the located
    // record. Wraps store.BeginContributionSweep()/EndContributionSweep(), so a
    // parameter whose bindings all detached returns to its base (the detach
    // edge). Rebuilt every tick; `emitters` scopes each voice's Variable reads.
    void EvaluateEffectParamBindings(const ParamSystem& params,
                                     EffectParamTargetStore& store,
                                     const EmitterTable* emitters);
    void EvaluateDirtyEffectParamBindings(const ParamSystem& params,
                                         EffectParamTargetStore& store,
                                         const EmitterTable* emitters);

    // Evaluates every BUS-scope attachment into the store's public Variable
    // slot: per bus target the contributions accumulate
    // in-domain (state offsets, environment offsets and sidechain
    // attenuation sum in dB — commutative, order-free), resolving each
    // definition's Variable at GLOBAL scope only (a bus has no emitter
    // context, section 5.3).
    // Rebuilt from scratch every tick; per-attachment change detection
    // marks each moved target dirty in the store with the changed
    // contributors' transitionMs folded (section 9.2), which is what the
    // bus flush publishes. Frozen (section 11.5) attachments contribute
    // neutral. A bus with no attachments never writes a slot and never
    // marks anything — the M5 no-binding bus path publishes nothing and
    // stays bit-identical. Consumes the store's pending-input marks (this
    // pass IS the evaluation those marks await).
    void EvaluateBusAttachments(const ParamSystem& params,
                                BusTargetStore& store);
    void EvaluateDirtyBusAttachments(const ParamSystem& params, BusTargetStore& store);

    bool BeginVariableProjection() noexcept;
    void RestoreVariableProjection() noexcept;
    std::uint64_t VariableProjectionStorageBytes() const noexcept;

    // Marks the store's pending-input demand for every BUS attachment
    // driven by `variableId` — the bus-side twin of
    // ParamSystem::NoteVariableWrite: a changed global write grows the
    // admission reservation's live census the moment it lands; the next
    // evaluation realizes (or disproves) it. Called by the global write
    // paths (bus attachments resolve at global scope only, so
    // emitter-local writes never mark here).
    void NoteBusVariableWrite(VnVariableId variableId,
                              BusTargetStore& store) noexcept;

    template <typename Fn>
    void ForEachBusVariableTarget(VnVariableId variableId, Fn&& fn) const {
        for (const Attachment& attachment : m_attachments) {
            if (!attachment.used || attachment.scopeKind != kTargetScopeBus) {
                continue;
            }
            const Definition& definition =
                m_definitions[attachment.definitionIndex];
            if (definition.sourceKind == BindingSourceKind::Variable &&
                definition.variableId == variableId) {
                fn(attachment.busIndex, definition.targetId);
            }
        }
    }

    template <typename Fn>
    void ForEachEffectVariableTarget(VnVariableId variableId, Fn&& fn) const {
        for (const Attachment& attachment : m_attachments) {
            if (!attachment.used ||
                attachment.scopeKind != VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                continue;
            }
            const Definition& definition =
                m_definitions[attachment.definitionIndex];
            if (definition.sourceKind != BindingSourceKind::Variable ||
                definition.variableId != variableId) {
                continue;
            }
            const EffectParamLocator locator =
                UnpackEffectParamLocator(definition.targetId);
            fn(EffectParamTargetStore::Scope::Bus,
               static_cast<std::uint64_t>(attachment.busIndex), locator.slot,
               locator.paramIndex, locator.effectType);
        }
        for (const VoiceEffectInstance& instance : m_voiceEffectInstances) {
            if (!instance.used || instance.pending || instance.parked) {
                continue;
            }
            const Definition& definition =
                m_definitions[instance.definitionIndex];
            if (definition.sourceKind != BindingSourceKind::Variable ||
                definition.variableId != variableId) {
                continue;
            }
            const EffectParamLocator locator =
                UnpackEffectParamLocator(definition.targetId);
            fn(EffectParamTargetStore::Scope::Voice,
               static_cast<std::uint64_t>(instance.renderHandle), locator.slot,
               locator.paramIndex, locator.effectType);
        }
    }

    // Every BUS-scope attachment with its definition, immutable — the
    // section-11.5 loop scan walks control edges through this.
    template <typename Fn>
    void ForEachBusAttachment(Fn&& fn) const {
        for (const Attachment& a : m_attachments) {
            if (a.used && a.scopeKind == kTargetScopeBus &&
                m_definitions[a.definitionIndex].sourceKind ==
                    BindingSourceKind::Variable) {
                fn(a, m_definitions[a.definitionIndex]);
            }
        }
    }

    // Mutable twin, for the loop scan's freeze/unfreeze application.
    template <typename Fn>
    void ForEachBusAttachmentMutable(Fn&& fn) {
        for (Attachment& a : m_attachments) {
            if (a.used && a.scopeKind == kTargetScopeBus &&
                m_definitions[a.definitionIndex].sourceKind ==
                    BindingSourceKind::Variable) {
                const bool frozen = a.meterFrozen;
                fn(a, m_definitions[a.definitionIndex]);
                if (a.meterFrozen != frozen) m_dependencies.Mark(a.dependency);
            }
        }
    }

    std::uint32_t DefinitionCount() const { return m_definitionCount; }
    std::uint32_t AttachmentCount() const { return m_attachmentCount; }
    std::uint32_t DefinitionCapacity() const {
        return static_cast<std::uint32_t>(m_definitions.size());
    }
    std::uint32_t AttachmentCapacity() const {
        return static_cast<std::uint32_t>(m_attachments.size());
    }
    bool DefinitionUsedAt(std::uint32_t slotIndex) const {
        return slotIndex < m_definitions.size() &&
               m_definitions[slotIndex].used;
    }
    bool AttachmentUsedAt(std::uint32_t slotIndex) const {
        return slotIndex < m_attachments.size() &&
               m_attachments[slotIndex].used;
    }
    std::uint32_t DefinitionSlotOf(BindingId id) const {
        const Definition* def = Find(id);
        return def != nullptr
                   ? static_cast<std::uint32_t>(
                         def - m_definitions.data())
                   : 0xFFFFFFFFu;
    }

    // Pool observability (section 12): high-water marks maintained at each
    // successful growth, refusal counters incremented exactly where the
    // pool-exhausted log record emits — counter and record stay 1:1.
    std::uint64_t DefinitionHighWater() const { return m_definitionHighWater; }
    std::uint64_t DefinitionRefusedCount() const {
        return m_definitionRefusedCount;
    }
    std::uint64_t AttachmentHighWater() const { return m_attachmentHighWater; }
    std::uint64_t AttachmentRefusedCount() const {
        return m_attachmentRefusedCount;
    }

    // Every Variable attachment with its definition, both scopes, pool
    // index order — the capture enumeration (section 12). Probe rows are
    // private and enumerate through the probe entries only. fn(const
    // Attachment&, const Definition&).
    template <typename Fn>
    void ForEachAttachment(Fn&& fn) const {
        for (const Attachment& a : m_attachments) {
            if (a.used && m_definitions[a.definitionIndex].sourceKind ==
                              BindingSourceKind::Variable) {
                fn(a, m_definitions[a.definitionIndex]);
            }
        }
    }

private:
    static BindingDependencyIdentity ReadAttachmentDependency(const void* fact, BindingDependencyKind kind) noexcept;
    static BindingDependencyIdentity ReadEffectDependency(const void* fact, BindingDependencyKind kind) noexcept;
    bool PrepareAttachmentDependency(std::uint32_t slot, std::uint32_t scope) noexcept;
    void RegisterAttachmentDependency(Attachment& attachment) noexcept;
    bool PrepareEffectDependency(std::uint32_t slot) noexcept;
    std::uint32_t EffectDependencySlot(const VoiceEffectInstance& instance) const noexcept;
    void RegisterEffectDependency(VoiceEffectInstance& instance) noexcept;
    void RemoveEffectDependency(VoiceEffectInstance& instance) noexcept;
    bool RefreshEffectDependencyEmitter(VoiceEffectInstance& instance, VnEmitterHandle emitter) noexcept;
    void* m_voiceCatalogContext = nullptr;
    bool (*m_prepareVoiceCatalog)(void*) noexcept = nullptr;
    BindingDependencyIndex m_dependencies;
    std::uint32_t m_renderBindingCapacity = 0;
    Definition* FindMutable(BindingId id);
    const Attachment* FindAttachment(BindingId id, std::uint32_t scopeKind,
                                     VnObjectId objectId) const;
    // Shared registration/replace validation: curve rules, known scoped
    // target, section-9.1 transition legality. Fills *outCurve on
    // success; refuses VN_INVALID_ARG with no state touched otherwise.
    static VnResult ValidateDescriptor(std::uint32_t scopeKind,
                                       std::uint32_t targetId,
                                       const CurvePoint* points,
                                       const CurveHandlePair* handles,
                                       std::uint32_t pointCount,
                                       std::uint32_t transitionMs,
                                       Curve* outCurve);

    std::vector<Definition> m_definitions; // fixed capacity (maxBindings)
    std::vector<Attachment> m_attachments; // fixed (maxBindingAttachments)
    struct AttachmentEvaluation {
        float lastContribution = 0.0f;
        bool lastHadValue = false;
    };
    std::vector<AttachmentEvaluation> m_projectionAttachments;
    bool m_projectionActive = false;
    // Voice-effect apply instances, fixed capacity (sized to maxAttachments).
    std::vector<VoiceEffectInstance> m_voiceEffectInstances;
    std::uint32_t m_definitionCount = 0;
    std::uint32_t m_attachmentCount = 0;
    std::uint32_t m_voiceEffectInstanceCount = 0;
    std::uint64_t m_definitionHighWater = 0;
    std::uint64_t m_definitionRefusedCount = 0;
    std::uint64_t m_attachmentHighWater = 0;
    std::uint64_t m_attachmentRefusedCount = 0;
};

struct Engine;

// Engine-session entries of the binding seam, mirroring the public verbs
// one for one: the C entry points unpack their descriptors (structSize
// vintages, curve arrays) and forward here — semantics, validation order,
// and result codes live at this layer. All take the public object
// vocabulary: playable ids, bus ids, PUBLIC voice handles.
VnResult RegisterBinding(Engine& engine, BindingId id,
                         VnVariableId variableId, std::uint32_t scopeKind,
                         std::uint32_t targetId, const CurvePoint* points,
                         const CurveHandlePair* handles, std::uint32_t pointCount,
                         std::uint32_t transitionMs,
                         std::uint64_t ownerCookie = 0);
VnResult UnregisterBinding(Engine& engine, BindingId id);
VnResult ReplaceBinding(Engine& engine, BindingId id, VnVariableId variableId,
                        std::uint32_t scopeKind, std::uint32_t targetId,
                        const CurvePoint* points, const CurveHandlePair* handles,
                        std::uint32_t pointCount, std::uint32_t transitionMs);
VnResult AttachBinding(Engine& engine, BindingId id, std::uint32_t scopeKind,
                       VnObjectId objectId);
VnResult DetachBinding(Engine& engine, BindingId id, std::uint32_t scopeKind,
                       VnObjectId objectId);
VnResult ApplyVoiceBinding(Engine& engine, VnVoiceHandle publicHandle,
                           BindingId id);
VnResult ClearVoiceBinding(Engine& engine, VnVoiceHandle publicHandle,
                           BindingId id);
VnResult CapturePropertyTargets(Engine& engine, VnPropertyTargetInfo* out,
                                std::uint32_t capacity,
                                std::uint32_t* requiredCount);
VnResult CapturePropertyBindings(Engine& engine, VnPropertyBindingInfo* out,
                                 std::uint32_t capacity,
                                 std::uint32_t* requiredCount);

} // namespace harken

#endif // HARKEN_PARAM_BINDING_TABLE_H
