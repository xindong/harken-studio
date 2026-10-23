// Logic-pack ledger: which logic entries each loaded v2 pack applied,
// plus the inter-pack dependency edges the closure check resolved
// through. The loader fills a record at apply, the unload path replays
// it in reverse through the existing unregister seams, and provider
// protection scans the reverse edges. One record per pack-table slot,
// keyed by the same generational handle; the audio thread never sees
// this. Logic thread only.
//
// This header is internal (api/src, never installed) like Engine.h; the
// test harness may include it to reach the fault-injection seam below.
#ifndef HARKEN_API_PACK_LOGIC_STORE_H
#define HARKEN_API_PACK_LOGIC_STORE_H

#include <harken/Harken.h>

#include "harken/core/CommandQueue.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"
#include "harken/media/PackTable.h"
#include "harken/param/Curve.h"
#include "harken/param/BindingTable.h"
#include "harken/param/EffectParamTargetStore.h"
#include "harken/param/ParamSystem.h"
#include "harken/param/Spatializer.h"
#include "harken/render/BusTree.h"
#include "harken/render/DynamicRoute.h"
#include "harken/render/EffectPool.h"

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace harken {

struct UnpackedBindingCurve {
    CurvePoint points[kMaxCurvePoints];
    CurveHandlePair handles[kMaxCurvePoints - 1];
    std::uint32_t pointCount = 0;
};

// One loaded logic-bearing pack's applied entries, in apply order per
// kind. Buses retain their full authored identity so replace can validate
// the live id/parent/cookie triple. Other unregister seams key on the
// public id. Buses/Sends are resident-only but reversibly unwound when the
// revision contains only the proven reversible infrastructure set.
// Attenuations are resident-only and have no unregister verb.
struct PackLogicRecord {
    bool used = false;
    VnPackHandle handle = 0;
    std::uint32_t packKind = kPackKindContent;

    std::vector<PackBusEntry> buses;
    std::vector<PackBusSendEntry> busSends;
    std::vector<PackBusEffectEntry> busEffects;
    std::vector<PackEffectDynamicRouteEntry> dynamicRoutes;
    std::vector<VnAttenuationId> attenuations;
    std::vector<VnBindingId> bindingDefinitions;
    std::vector<PackBindingAttachmentEntry> bindingAttachments;
    std::vector<PackLogicImage::VoiceRecipe> voiceRecipes;

    // Provider packs this pack's closure resolved external references
    // through (deduplicated, never the pack itself). Unloading or
    // replacing a provider that still appears in any loaded record
    // refuses VN_IN_USE with the consumer named in the log.
    std::vector<VnPackHandle> providers;

    // Directory stamps of the loaded revision (kind, id, payload
    // checksum). Replace staging diffs the incoming revision against
    // them: same id + same checksum = unchanged = no-op.
    std::vector<PackLogicImage::Stamp> stamps;

    void Reset() noexcept { *this = PackLogicRecord{}; }
};

static_assert(std::is_nothrow_move_assignable_v<PackLogicRecord>);

struct PackBusEffectReservation {
    std::size_t imageIndex = 0;
    EffectInstanceRef ref;
};

// One staged pack revision: the complete immutable new image (with its
// media buffer, dependency edges, and the reservations held across the
// gap) plus the diff plan the activation replays — per entry kind, the
// section-19.5 semantics. Staged by Vn_ReplacePack after whole-revision
// staging succeeds; activated at the fixed Vn_Update phase; discarded
// whole on a late failure with the old revision intact.
struct PackPendingRevision {
    VnPackHandle handle = 0;
    std::uint32_t packKind = kPackKindContent;
    PackLogicImage image;
    PackTable::AlignedBuffer bytes;
    std::vector<VnPackHandle> providers;

    // Diff plan. Additions and swaps index into `image`'s sections;
    // removals carry the OLD revision's ids/entries from the ledger.
    // Identity-changed binding definitions appear as remove + add, never
    // in-place (the section-5.5 structural-identity rule), and drag
    // their kept attachments into remove + add with them.
    std::vector<std::size_t> addBuses; // image.buses, parent-first order
    std::vector<PackBusEntry> removeBuses;
    std::vector<std::size_t> reparentBuses;
    std::vector<PackBusSendEntry> addSends;
    std::vector<PackBusSendEntry> removeSends;
    std::vector<PackBusSendEntry> relevelSends; // same edge, new level
    std::vector<std::size_t> addBusEffects;
    std::vector<std::size_t> reviseBusEffects;
    std::vector<std::size_t> replaceBusEffects;
    std::vector<PackBusEffectEntry> removeBusEffects;
    std::vector<std::size_t> addDynamicRoutes;
    std::vector<std::size_t> reviseDynamicRoutes;
    std::vector<PackEffectDynamicRouteEntry> removeDynamicRoutes;
    std::vector<std::size_t> addAttenuations;
    std::vector<std::size_t> reviseAttenuations;
    std::vector<std::size_t> addBindingDefs;
    std::vector<std::size_t> reviseBindingDefs;
    std::vector<VnBindingId> removeBindingDefs;
    std::vector<UnpackedBindingCurve> bindingCurves;
    std::vector<PackBindingAttachmentEntry> addAttachments;
    std::vector<PackBindingAttachmentEntry> removeAttachments;

    // Reservations held across the gap: the plan slot is acquired at the
    // call; command admission is re-checked at activation (the queue
    // drains between ticks — a shortage defers the activation one tick,
    // never splits it); the section-10.4 growth covers the added
    // attachments through the transition.
    bool needsPlan = false;
    std::uint32_t planIndex = 0xFFFFFFFFu;
    std::uint32_t dynamicPlanIndex = 0xFFFFFFFFu;
    std::uint32_t levelCommands = 0;
    std::uint32_t grownAttachments = 0;

    // The post-activation ledger content, fully built (every vector
    // populated) during the call's staging so the activation only MOVES
    // it into the slot — the apply path allocates nothing for
    // bookkeeping and cannot throw between mutations.
    PackLogicRecord stagedRecord;

    // The pack table's media entry records, prebuilt during the call's
    // staging for the same reason: the activation's media swap only
    // stamps registration cookies into them and moves them into the
    // slot. A swap that fails before its commit tail leaves them (and
    // `bytes`) intact, so the revision can retry next tick whole.
    std::vector<PackEntry> mediaEntries;
    std::vector<PackBusEffectReservation> busEffectRefs;
    std::vector<DynamicRouteDefinition> dynamicRouteCandidates;
    StableStoragePreparation dynamicBusTapBacking;
    std::vector<std::uint32_t> dynamicRouteRemovals;
    std::vector<CommandRecord> activationCommands;
    std::vector<BusTree::PreparedBusSlot> preparedBusSlots;
    std::vector<EffectParamTargetStore::PreparedSeedToken>
        effectParamSeedTokens;
    std::vector<Spatializer::PreparedProfileToken>
        preparedAttenuationProfiles;
    std::vector<BindingTable::PreparedAttachmentToken>
        preparedBindingAttachmentRemovals;
    std::vector<BindingTable::PreparedDefinitionToken>
        preparedBindingDefinitionRemovals;
    std::vector<BindingTable::PreparedDefinitionToken>
        preparedBindingDefinitionAdds;
    std::vector<BindingTable::PreparedDefinitionToken>
        preparedBindingDefinitionRevisions;
    std::vector<BindingTable::PreparedAttachmentToken>
        preparedBindingAttachmentAdds;
    ParamSystem::PreparedPurgeToken preparedBindingPurge;
    std::vector<ParamSystem::PreparedPurgeExit>
        preparedBindingPurgeExits;
    std::vector<VnObjectId> preparedBindingPurgeDefinitionIds;
    std::vector<std::uint8_t> bindingDefinitionOccupancy;
    std::vector<std::uint8_t> bindingAttachmentOccupancy;
    bool effectParamSeedsPrepared = false;
    bool activationAttemptPrepared = false;
    std::uint64_t activationAttemptTopologyPlanCount = 0;

    // Replace activation scratch is allocated while the revision is staged,
    // never after its media transaction. The prospective arrays describe the
    // complete Bus/Send graph without mutating the live mirrors; the sort
    // arrays let topology construction stay allocation-free on every retry.
    std::vector<VnBusId> prospectiveBusIds;
    std::vector<std::uint32_t> prospectiveBusGenerations;
    std::vector<std::uint32_t> prospectiveParents;
    std::vector<std::uint8_t> prospectiveAlive;
    std::vector<std::uint32_t> prospectiveSendTargets;
    std::vector<VnPackHandle> prospectiveSendOwners;
    std::vector<std::uint64_t> prospectiveSendTopologyOwners;
    std::vector<float> prospectiveSendLevels;
    std::vector<float> prospectiveSendLevelsDb;
    std::vector<std::uint8_t> prospectiveSendLevelApply;
    std::vector<float> prospectiveBusVolumeDb;
    std::vector<float> prospectiveBusOutputVolumeDb;
    std::vector<std::uint8_t> prospectiveBusVolumeApply;
    std::vector<std::uint32_t> topologyFeederCount;
    std::vector<std::uint32_t> topologyEmittedFeeders;
    std::vector<std::uint8_t> topologyEmitted;
    std::uint32_t prospectiveBusCount = 0;
};

// Fixed-capacity store indexed by pack-table slot (the handle's low 32
// bits), sized to maxPacks at engine construction. Generation checks
// ride the stored handle, so a slot reused by a later v1-only load never
// aliases a stale logic record.
class PackLogicStore {
public:
    class PreparedClaimToken {
    public:
        bool IsPrepared() const noexcept { return m_owner != nullptr; }
        VnPackHandle Handle() const noexcept { return m_handle; }

    private:
        friend class PackLogicStore;
        PackLogicStore* m_owner = nullptr;
        std::uint32_t m_slot = 0;
        VnPackHandle m_handle = 0;
    };

    explicit PackLogicStore(std::uint32_t capacity)
        : m_records(capacity) {}

    PackLogicRecord* RecordFor(VnPackHandle handle) {
        const std::uint32_t slot = static_cast<std::uint32_t>(handle);
        if (slot >= m_records.size()) {
            return nullptr;
        }
        PackLogicRecord& record = m_records[slot];
        if (!record.used || record.handle != handle) {
            return nullptr;
        }
        return &record;
    }

    const PackLogicRecord* RecordFor(VnPackHandle handle) const {
        return const_cast<PackLogicStore*>(this)->RecordFor(handle);
    }

    // Freezes the exact ledger slot paired with an initial Pack token.
    // Commit is a non-refusing move after the media transaction succeeds.
    VnResult PrepareClaim(VnPackHandle handle,
                          PreparedClaimToken* out) noexcept {
        if (out == nullptr) {
            return VN_INVALID_ARG;
        }
        *out = PreparedClaimToken{};
        const std::uint32_t slot = static_cast<std::uint32_t>(handle);
        if (slot >= m_records.size()) {
            return VN_INVALID_CONFIG;
        }
        if (m_records[slot].used) {
            return VN_IN_USE;
        }
        out->m_owner = this;
        out->m_slot = slot;
        out->m_handle = handle;
        return VN_OK;
    }

    void CancelPreparedClaim(PreparedClaimToken* token) noexcept {
        if (token != nullptr) {
            *token = PreparedClaimToken{};
        }
    }

    void CommitPreparedClaim(PreparedClaimToken* token,
                             PackLogicRecord&& content) noexcept {
        assert(token != nullptr && token->m_owner == this &&
               token->m_slot < m_records.size() &&
               !m_records[token->m_slot].used);
        PackLogicRecord& record = m_records[token->m_slot];
        record = std::move(content);
        record.used = true;
        record.handle = token->m_handle;
        *token = PreparedClaimToken{};
    }

    void Release(VnPackHandle handle) noexcept {
        PackLogicRecord* record = RecordFor(handle);
        if (record != nullptr) {
            record->Reset();
        }
    }

    // First loaded consumer depending on `provider`, or 0. Provider
    // protection names it in the refusal log.
    VnPackHandle ConsumerOf(VnPackHandle provider) const {
        for (const PackLogicRecord& record : m_records) {
            if (!record.used) {
                continue;
            }
            for (VnPackHandle p : record.providers) {
                if (p == provider) {
                    return record.handle;
                }
            }
        }
        return 0;
    }

    template <typename Fn>
    void ForEachRecord(Fn&& fn) const {
        for (const PackLogicRecord& record : m_records) {
            if (record.used) {
                fn(record);
            }
        }
    }

    const PackLogicImage::VoiceRecipe* FindVoiceRecipe(
        VnSoundId playableId, VnPackHandle* outOwner = nullptr) const {
        for (const PackLogicRecord& record : m_records) {
            if (!record.used) {
                continue;
            }
            for (const PackLogicImage::VoiceRecipe& recipe :
                 record.voiceRecipes) {
                if (recipe.header.playableId == playableId) {
                    if (outOwner != nullptr) {
                        *outOwner = record.handle;
                    }
                    return &recipe;
                }
            }
        }
        if (outOwner != nullptr) {
            *outOwner = 0;
        }
        return nullptr;
    }

    // Staged revisions awaiting their activation tick, in call order
    // (same-tick multiple replaces of DIFFERENT packs activate in call
    // order; a second replace of the SAME pack while one is pending
    // refuses upstream). Logic thread only; heap growth is logic-side
    // registration-time discipline.
    std::vector<PackPendingRevision> pending;

    PackPendingRevision* PendingFor(VnPackHandle handle) {
        for (PackPendingRevision& revision : pending) {
            if (revision.handle == handle) {
                return &revision;
            }
        }
        return nullptr;
    }

private:
    std::vector<PackLogicRecord> m_records;
};

static_assert(
    std::is_nothrow_move_assignable_v<
        PackLogicStore::PreparedClaimToken>);

// --- test-only fault injection -----------------------------------------
// Always-compiled internal seam (an integer compare per staging step;
// never part of the C ABI). A nonzero step forces the v2 loader's
// staging to fail AT that step, proving the whole-load contract: the
// refusal surfaces as the step's own result code with zero mutations,
// and for a replace the old revision survives intact. Cleared manually
// by the test; production code never sets it.
enum class PackStagingFault : std::uint32_t {
    None = 0,
    AfterParse = 1,       // fail right after image parse/validation
    AfterClosure = 2,     // fail after the closure check
    AfterConflicts = 3,   // replace only, after DiffPackRevision
    AfterCapacity = 4,    // fail after pool/queue capacity staging
    BeforeApply = 5,      // the LAST gate: everything staged, apply next
    ActivationProspectiveTopology = 6,
    ActivationQueueReservation = 7,
    ActivationMediaPreparation = 8,
    InitialCommandScratchAllocation = 9,
    InitialPlanPreparedBeforeQueueReservation = 10,
    InitialQueueReservation = 11,
    InitialMediaPreparation = 12,
    PartitionDecode = 13,
    CatalogIndex = 14,
    CompositeReserve = 15,
    CompositionPublish = 16,
    AfterAllPreparedBeforeCommit = 17
};

void SetPackStagingFaultForTests(PackStagingFault fault);
PackStagingFault PackStagingFaultForTests();

// --- v2 loader entry points (defined in HarkenApi.cpp beside the shared
// staging kernel they reuse: topology sort, plan publish, command
// admission, meter influence walk) ---

struct Engine;

bool PeekPackHeader(const char* utf8Path, std::uint32_t* outVersion,
                    std::uint32_t* outHeaderBytes);

// The section-19.4 staged, all-or-nothing v2 load: parse -> closure ->
// id conflicts -> every capacity (pools, variable table, topology-plan
// slot, command-queue admission) -> apply. Every failure mode is
// exhausted before the first mutation; the apply path cannot refuse for
// business reasons. Logic thread only.
VnResult LoadPackV2(Engine& engine, const char* utf8Path,
                    VnPackHandle* outHandle);

// The v2 unload: refused on resident packs (dedicated readable
// refusal) and on providers with loaded consumers (VN_IN_USE, consumer
// named in the log); a content pack unloads fully symmetrically through
// the existing unregister seams, whole or not at all. Logic thread
// only.
VnResult UnloadPackV2(Engine& engine, VnPackHandle handle);

// The v2 replace: stages a complete immutable pack revision (rollback,
// ownership and dependency information included) after exhausting every
// failure mode at the call, then returns; the revision activates whole
// at the fixed Vn_Update phase. Every lookup before the activation tick
// sees the OLD revision in full. A late staging failure discards the
// staged revision with the old one intact. Logic thread only.
VnResult ReplacePackV2(Engine& engine, VnPackHandle handle,
                       const char* utf8Path);

VnResult LoadCanonicalPack(Engine& engine, const VnPackLoadDesc* desc,
                           VnPackHandle* outHandle);
VnResult LoadProvisionalCanonicalContentForTests(
    Engine& engine, const VnPackLoadDesc* desc, VnPackHandle* outHandle);
VnResult UnloadCanonicalPack(Engine& engine, VnPackHandle handle,
                             std::uint32_t flags);
VnResult ReplaceCanonicalPack(Engine& engine, VnPackHandle handle,
                              const VnPackLoadDesc* desc);

// The fixed activation phase (installed immediately before the binding
// revision install inside Vn_Update, so a revision's binding replaces
// land in the same tick): applies every pending revision in call order
// — media generation swap, per-kind section-19.5 semantics, ONE
// topology republish when the bus graph changed. A command-admission
// shortage defers a revision to the next tick whole; an activation-time
// re-screen failure (world state drifted during the gap) discards it
// whole with a named log record and the old revision intact.
void ActivatePendingPackRevisions(Engine& engine);

} // namespace harken

#endif // HARKEN_API_PACK_LOGIC_STORE_H
