// Pack table: loaded .pack instances behind generational pack
// handles. Loading reads the whole file into a pack-owned 64-byte-aligned
// buffer on the logic thread (file I/O is legal there; the audio thread is
// never involved), validates the container end to end, and registers every
// entry into the PCM registry in reference mode — the pack buffer stays
// pinned while loaded. Load is all-or-nothing: any validation or
// registration failure leaves the registry exactly as it was.
//
// The audio thread never touches this table. Voice liveness flows through
// the registry entries' atomic use counters, which is also what gates
// unload: while any entry the pack still owns has a playing voice the
// whole pack refuses with VN_IN_USE. Ownership is provenance, not id:
// each registration carries a registry cookie, and the pack acts only on
// entries whose cookie still matches (ids are caller-reusable).
//
// Pack handles use the voice-handle scheme: low 32 bits slot index, high
// 32 bits generation, generation starts at 1 and bumps on unload (skipping
// 0), so zero is never a valid handle and a stale handle is a benign no-op
// returning VN_STALE_HANDLE with a dedicated counter.
#ifndef HARKEN_MEDIA_PACK_TABLE_H
#define HARKEN_MEDIA_PACK_TABLE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/abi/PackRevisionIdentity.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PcmRegistry.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace harken {

// Pack table capacity is a runtime config choice (VnConfig.maxPacks);
// the default lives with every other capacity default in EngineConfig.

// One pack entry's identity and loop metadata, kept after load so later
// playback features (loop region resolution) can reach it without
// re-reading the file. Loop semantics themselves are not interpreted here.
// registrationCookie is the registry's provenance stamp for the
// registration this pack made: ids are caller-reusable, so every unload
// or metadata decision first proves the entry under the id is still the
// pack's own by matching this cookie against the registry's current one.
struct PackEntry {
    VnSoundId soundId{};
    std::uint32_t flags = 0;          // kPackFlag* bits
    std::uint64_t frameCount = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;   // 0 = whole file when loopable
    std::uint64_t registrationCookie = 0;
    PCMRegistry::PackReference registration;
};

class PackTable {
public:
    class PreparedLoadToken {
    public:
        bool IsPrepared() const noexcept { return m_owner != nullptr; }
        VnPackHandle Handle() const noexcept { return m_handle; }

    private:
        friend class PackTable;
        PackTable* m_owner = nullptr;
        std::uint32_t m_slotIndex = 0;
        std::uint32_t m_generation = 0;
        VnPackHandle m_handle = 0;
    };

    // Slots are preallocated at construction; capacity is fixed for the
    // engine's life like every other pool.
    explicit PackTable(std::uint32_t capacity,
                       std::size_t retirementCapacity = 0,
                       std::size_t resourceClosureCapacity = 0);

    // Pack buffers hold reference-mode sample data, so blob addresses must
    // honor the container's 64-byte alignment guarantee in memory too.
    // Public because the v2 loader reads the file itself (it has to see
    // the format version before choosing a branch) and hands the buffer
    // over through LoadPrepared below.
    struct AlignedDeleter {
        void operator()(std::uint8_t* p) const;
    };
    using AlignedBuffer = std::unique_ptr<std::uint8_t[], AlignedDeleter>;

    class PreparedReplaceToken {
    public:
        bool IsPrepared() const noexcept { return m_owner != nullptr; }
        VnPackHandle Handle() const noexcept { return m_handle; }

    private:
        friend class PackTable;
        PackTable* m_owner = nullptr;
        VnPackHandle m_handle = 0;
        AlignedBuffer m_bytes;
        std::vector<PackTocEntry> m_parsed;
        std::vector<PackEntry> m_entries;
    };

    // Move-only identity for the fixed retirement row reserved by
    // ReplacePreparedV2. The row itself owns all predecessor and restore data.
    struct RetiredReplace {
        RetiredReplace() = default;
        RetiredReplace(const RetiredReplace&) = delete;
        RetiredReplace& operator=(const RetiredReplace&) = delete;
        RetiredReplace(RetiredReplace&&) = default;
        RetiredReplace& operator=(RetiredReplace&&) = default;

        bool IsPrepared() const noexcept {
            return rowIndex != std::numeric_limits<std::size_t>::max();
        }
        std::size_t RowIndex() const noexcept { return rowIndex; }
        std::uint64_t RowGeneration() const noexcept { return rowGeneration; }

    private:
        friend class PackTable;
        std::size_t rowIndex = std::numeric_limits<std::size_t>::max();
        std::uint64_t rowGeneration = 0;
    };

    class PreparedMutation {
    public:
        PreparedMutation() = default;
        PreparedMutation(const PreparedMutation&) = delete;
        PreparedMutation& operator=(const PreparedMutation&) = delete;
        PreparedMutation(PreparedMutation&&) noexcept = default;
        PreparedMutation& operator=(PreparedMutation&&) noexcept = default;

        bool IsPrepared() const noexcept { return m_owner != nullptr; }
        bool IsUnload() const noexcept { return m_unload; }
        PackResourceRevisionToken SuccessorResourceToken() const noexcept {
            return m_resourceToken;
        }
        const RetiredReplace& Predecessor() const noexcept {
            return m_predecessor;
        }

    private:
        friend class PackTable;
        PackTable* m_owner = nullptr;
        VnPackHandle m_handle = 0;
        bool m_unload = false;
        PackRevisionKey m_revisionKey{};
        PackResourceRevisionToken m_resourceToken = 0;
        AlignedBuffer m_bytes;
        std::vector<PackEntry> m_entries;
        PCMRegistry::PreparedPackMutation m_pcm;
        RetiredReplace m_predecessor;
    };

    // Predicate reporting whether a sound id is already owned by a playable
    // surface outside the PCM registry — a container or a registered stream in
    // the flat playable-ID space, any owner this table's registry cannot see.
    // Kept as a caller-supplied function pointer so the media module does not
    // depend on those surfaces. nullptr = nothing outside the registry to check
    // (only registry duplicates gate the load).
    using ReservedIdFn = bool (*)(void* ctx, VnSoundId id);

    // Loads the pack at utf8Path and registers every entry in reference
    // mode. Returns VN_NO_PACK when the table is full, VN_BAD_PACK for an
    // unreadable or invalid container (any header, TOC, checksum, bounds,
    // or consistency failure refuses the whole pack), VN_IN_USE when an
    // entry's id is already registered or reserved by `isReserved` (the flat
    // playable-ID space admits no shadowing; the check runs before any
    // registration, so a refused load registers nothing), VN_OK with a handle
    // on success. Logic thread only.
    VnResult Load(const char* utf8Path, PCMRegistry& registry,
                  VnPackHandle* outHandle, ReservedIdFn isReserved = nullptr,
                  void* reservedCtx = nullptr);

    // Hot reload: replaces a loaded pack wholesale with the container at
    // utf8Path. The new file loads and validates fully first — any header,
    // TOC, checksum, bounds, or consistency failure is VN_BAD_PACK, and a new
    // entry id owned by a registration outside this pack (foreign registry
    // cookie, or reserved per `isReserved`) is VN_IN_USE — with the old pack
    // untouched in every refusal. On success the entries swap as ONE
    // generation: every old entry the pack still owns leaves the registry
    // immediately (a removed id fails new lookups from the moment this
    // returns), the new TOC registers in reference mode against the new
    // buffer, and the old buffer plus its extracted entries stay pinned as a
    // retiring generation until the last logical voice on them drains (none
    // pinned when nothing was playing). The pack handle survives unchanged.
    // Refusals: VN_STALE_HANDLE for a dead handle; VN_IN_USE while this
    // pack's previous old generation still drains (one retiring generation
    // per asset — the caller sweeps first). Logic thread only.
    VnResult Replace(VnPackHandle handle, const char* utf8Path,
                     PCMRegistry& registry, ReservedIdFn isReserved = nullptr,
                     void* reservedCtx = nullptr);
    VnResult PrepareReplace(VnPackHandle handle, const char* utf8Path,
                            const PCMRegistry& registry,
                            ReservedIdFn isReserved,
                            void* reservedCtx,
                            PreparedReplaceToken* out);
    VnResult CommitPreparedReplace(PreparedReplaceToken* token,
                                   PCMRegistry& registry);

    // Destroys every retiring pack generation whose entries have all
    // drained, releasing the old buffer. Logic thread only.
    using RetainResourceTokenFn =
        bool (*)(void*, PackResourceRevisionToken);
    struct CanonicalSetMember {
        VnPackHandle expectedHandle = 0;
        PackRevisionKey revisionKey{};
        const AlignedBuffer* bytes = nullptr;
        const std::vector<PackTocEntry>* media = nullptr;
    };

    class PreparedSetMutation {
    public:
        PreparedSetMutation() = default;
        ~PreparedSetMutation() noexcept;
        PreparedSetMutation(const PreparedSetMutation&) = delete;
        PreparedSetMutation& operator=(const PreparedSetMutation&) = delete;
        PreparedSetMutation(PreparedSetMutation&& other) noexcept;
        PreparedSetMutation& operator=(PreparedSetMutation&& other) noexcept;

        bool IsPrepared() const noexcept { return m_owner != nullptr; }
        VnPackHandle HandleAt(std::size_t member) const noexcept;
        PackResourceRevisionToken ResourceTokenAt(std::size_t member) const noexcept;

    private:
        friend class PackTable;
        struct Change {
            bool touched = false;
            bool hasSuccessor = false;
            bool retainAnchor = false;
            VnPackHandle handle = 0;
            PackRevisionKey revisionKey{};
            PackResourceRevisionToken resourceToken = 0;
            std::vector<PackEntry> entries;
            RetiredReplace predecessor;
        };
        struct RetiringMedia {
            VnSoundId id{};
            std::uint64_t cookie = 0;
            std::size_t slotIndex = 0;
        };
        PackTable* m_owner = nullptr;
        PCMRegistry* m_registry = nullptr;
        std::vector<Change> m_changes;
        std::vector<std::size_t> m_members;
        std::vector<RetiringMedia> m_retiringMedia;
        PCMRegistry::PreparedPackMutation m_pcm;
    };

    VnResult PrepareCanonicalSet(
        const std::vector<CanonicalSetMember>& members,
        const std::vector<VnPackHandle>& removed, PCMRegistry& registry,
        PreparedSetMutation* out,
        const std::vector<PackResourceRevisionToken>& retained = {});
    void CancelPreparedSet(PreparedSetMutation* prepared) noexcept;
    void CommitPreparedSet(PreparedSetMutation* prepared,
                           PCMRegistry& registry) noexcept;

    void SweepRetiring(RetainResourceTokenFn retain = nullptr,
                       void* retainCtx = nullptr);

    // Retiring pack generations still pinned by voices (the pack share of
    // the snapshot's pinnedGenerationCount).
    std::size_t RetiringCount() const;

    // Whether the pack behind this handle still has a retiring generation.
    bool HasRetiring(VnPackHandle handle) const;

    // Unregisters every entry this pack still owns and releases the pack
    // buffer. Ownership is decided per entry by the registration cookie:
    // an entry the caller removed individually (or removed and replaced
    // with a registration of their own) no longer matches and is left
    // alone — it neither gates the unload nor gets unregistered. Refused
    // with VN_IN_USE while any still-owned entry has a playing voice
    // (nothing is unregistered on refusal). A stale handle is a benign
    // no-op returning VN_STALE_HANDLE with the counter incremented.
    // Logic thread only.
    VnResult Unload(VnPackHandle handle, PCMRegistry& registry);

    // Entry metadata of a loaded pack; nullptr for a stale handle. The
    // pointer stays valid until the pack unloads. This is the pack's own
    // record of what it loaded; whether an entry still owns its registry
    // id is a cookie question (see FindEntryBySound).
    const std::vector<PackEntry>* Entries(VnPackHandle handle) const;

    // Loop metadata lookup across all loaded packs for playback features.
    // Returns the entry only while its registration cookie still matches
    // the registry's current one — metadata is never served for an id the
    // caller has since re-registered with other content. nullptr
    // otherwise.
    const PackEntry* FindEntryBySound(VnSoundId soundId,
                                           const PCMRegistry& registry) const;

    std::uint32_t LoadedCount() const;
    std::uint64_t StaleHandleCount() const { return m_staleHandleHits; }

    // --- v2 branch additions (logic-bearing packs). The v1 load/replace/
    // unload paths above are untouched; these entry points serve the
    // engine-side v2 loader, which has already parsed and validated the
    // container itself (pack_logic_image) and proven every id vacant. ---

    // Fixed slot capacity, for the loader's staging preflight.
    std::uint32_t SlotCapacity() const {
        return static_cast<std::uint32_t>(m_slots.size());
    }

    // Books one refused-by-full-table load into the observability
    // counter when the v2 staging path detects the shortage itself.
    void NoteNoPack() { ++m_noPackCount; }

    // Freezes the exact slot, generation, and handle an initial v2 load
    // will consume. The token is a logic-thread reservation only: no Pack
    // or PCM becomes visible until LoadPrepared commits it.
    VnResult PrepareLoad(PreparedLoadToken* out) noexcept;
    void CancelPreparedLoad(PreparedLoadToken* token) noexcept;

    // Adopts an already-validated v2 buffer through the exact prepared
    // token: registers
    // every media record in reference mode against the buffer, and mints
    // the handle. The caller staged everything refusable (slot free, ids
    // vacant) AND prebuilt the entry storage, so this path allocates
    // nothing itself; the only remaining failure is a system allocation
    // throw inside RegisterPCM — handled with the v1 Load()'s
    // unwind-and-rethrow posture, and the buffer (taken by reference) is
    // consumed only on the success path, so a failed call leaves it with
    // the caller. `parsedMedia` may be empty (a resident pack): the slot
    // then pins no buffer and simply anchors the handle. `entries` carries
    // one prebuilt record per media entry, in `parsedMedia` order; the
    // registration stamps each cookie in place. Logic thread only.
    VnResult LoadPrepared(PreparedLoadToken* token, AlignedBuffer& bytes,
                          std::vector<PackEntry>&& entries,
                          const std::vector<PackTocEntry>& parsedMedia,
                          PCMRegistry& registry, VnPackHandle* outHandle,
                          const PackRevisionKey* revisionKey = nullptr);

    // The unload liveness gate alone, side-effect-free: VN_STALE_HANDLE
    // for a dead handle, VN_IN_USE while any still-owned entry has a
    // playing voice, VN_OK otherwise. The v2 unload path runs it inside
    // its whole-pack preflight so a media refusal can never surface after
    // logic entries were already unregistered. Logic thread only.
    VnResult CanUnload(VnPackHandle handle, const PCMRegistry& registry) const;

    // Non-failing commit tail for a v2 Resident pack whose caller has already
    // proven that the handle is live and the media entry set is empty. This is
    // intentionally narrower than Unload(): it performs no registry work and
    // has no refusal path, so a structural command batch can be published
    // before the pack slot is released without opening a second transaction.
    // Violating the precondition is an internal defect (Debug assertion).
    // Logic thread only.
    void CommitUnloadEmptyResident(VnPackHandle handle) noexcept;

    // Replace admission for the v2 revision path, side-effect-free: the
    // A dead handle is VN_STALE_HANDLE, and a new entry id owned by a
    // registration outside this pack (foreign cookie or reserved per
    // `isReserved`) is VN_IN_USE. Older canonical generations occupy fixed
    // retirement rows and do not block another replacement of this handle.
    // Logic thread only.
    VnResult CanReplacePreparedV2(VnPackHandle handle,
                                  const std::vector<PackTocEntry>& parsedMedia,
                                  const PCMRegistry& registry,
                                  ReservedIdFn isReserved = nullptr,
                                  void* reservedCtx = nullptr) const;

    VnResult PrepareRevisionMutation(
        VnPackHandle handle, AlignedBuffer& bytes,
        std::vector<PackEntry>& entries,
        const std::vector<PackTocEntry>& parsedMedia, PCMRegistry& registry,
        PreparedMutation* out,
        const PackRevisionKey* revisionKey = nullptr);
    VnResult PrepareUnloadMutation(VnPackHandle handle,
                                   const PCMRegistry& registry,
                                   PreparedMutation* out);
    void CancelPreparedMutation(PreparedMutation* prepared) noexcept;
    void CommitPreparedMutation(PreparedMutation* prepared,
                                PCMRegistry& registry,
                                RetiredReplace* outPredecessor = nullptr) noexcept;

    // The v2 revision swap: the v1 Replace()'s commit half against an
    // already-validated buffer + media set — retire every still-owned old
    // entry whole, register the new records in reference mode, swap the
    // buffer, pin the old generation until its voices drain. The caller
    // ran CanReplacePreparedV2 (and no interleaving mutation is possible
    // on the externally synchronized thread between its activation-time
    // re-check and this call), so only a system allocation throw remains
    // — and this runs at the ACTIVATION phase, where an exception must
    // not escape and the staged revision must survive for a retry. The
    // whole call is therefore transactional: every internal allocation
    // sits before the first registry mutation, any throw or refusal
    // unwinds the mutations already made and returns nonzero with the
    // buffer (by reference) and `entries` (by reference, cookies aside)
    // untouched in the caller's revision, and the buffer/entries are
    // consumed only at the noexcept commit tail. The handle survives
    // unchanged. A fixed retirement row is reserved before registry mutation.
    // When `outPredecessor` is set, its move-only identity lets the caller
    // either park the predecessor or restore it without any later allocation.
    // Logic thread only.
    VnResult ReplacePreparedV2(VnPackHandle handle, AlignedBuffer& bytes,
                               std::vector<PackEntry>& entries,
                               const std::vector<PackTocEntry>& parsedMedia,
                               PCMRegistry& registry,
                               RetiredReplace* outPredecessor = nullptr,
                               const PackRevisionKey* revisionKey = nullptr);

    // Parks a predecessor returned by ReplacePreparedV2 for the usual
    // drain path. A fully drained generation is released here.
    void ParkRetiredReplace(VnPackHandle handle, RetiredReplace&& retired,
                            bool retainAnchor);

    // Undoes a successful ReplacePreparedV2 that handed its predecessor to
    // the caller: the live successor mapping is extracted, the predecessor
    // buffer and pack entries become the live slot again, predecessor PCM
    // nodes are reinserted, and any in-use successor views stay pinned in the
    // reserved retirement row. ReplacePreparedV2 reserves every container this
    // path consumes, so the restore is allocation-free and cannot refuse.
    // Logic thread only.
    void RestorePreparedReplaceV2(VnPackHandle handle,
                                  RetiredReplace& predecessor,
                                  PCMRegistry& registry) noexcept;

    // The loaded pack whose still-owned registration carries this sound
    // id, or 0 when no loaded pack owns it (unregistered, re-registered
    // by the caller, or registered outside any pack). Provider-dependency
    // capture for cross-pack media references. Logic thread only.
    VnPackHandle OwnerOfSound(VnSoundId soundId,
                              const PCMRegistry& registry) const;
    VnSoundId ConflictingSoundId() const noexcept { return m_conflictingSoundId; }
    bool ActiveRevisionIdentity(
        VnPackHandle handle, PackRevisionKey* outKey,
        PackResourceRevisionToken* outToken) const noexcept;
    VnResult LookupExact(PackResourceRevisionToken resourceToken,
                         VnSoundId soundId, const PCMRegistry& registry,
                         PCMView* out) const noexcept;
    VnResult AcquireResourceClosure(
        const PackResourceRevisionToken* resourceTokens,
        std::size_t resourceTokenCount,
        PackResourceClosureToken* outClosureToken) noexcept;
    VnResult AcquirePreparedResourceClosure(
        const std::shared_ptr<const std::vector<PackResourceRevisionToken>>& resourceTokens,
        PackResourceClosureToken* outClosureToken) noexcept;
    VnResult ValidateResourceClosure(
        PackResourceClosureToken closureToken) const noexcept;
    VnResult ReleaseResourceClosure(
        PackResourceClosureToken closureToken) noexcept;
    VnResult LookupExactClosure(PackResourceClosureToken closureToken,
                                VnSoundId soundId,
                                const PCMRegistry& registry,
                                PCMView* out) const noexcept;
    VnResult SetResourceClosureGenerationForTests(
        std::size_t rowIndex, std::uint32_t generation) noexcept;
    VnResult SetSlotGenerationForTests(
        std::size_t rowIndex, std::uint32_t generation) noexcept;

    // Observability facts merged into snapshots: the most packs ever loaded
    // at once, and loads refused because every slot was taken.
    std::uint64_t LoadedHighWater() const { return m_loadedHighWater; }
    std::uint64_t NoPackCount() const { return m_noPackCount; }

private:
    struct Slot {
        bool loaded = false;
        std::uint32_t generation = 1;
        PackRevisionKey revisionKey{};
        PackResourceRevisionToken resourceToken = 0;
        std::uint64_t closureRefCount = 0;
        AlignedBuffer bytes;
        std::vector<PackEntry> entries;
    };

    // Fixed rows own replaced pack buffers and extracted registry nodes until
    // both PCM readers and exact resource-token anchors release them.
    enum class RetiringState : std::uint8_t {
        Free,
        Prepared,
        Live,
        Quarantined,
    };

    struct RetiringGeneration {
        RetiringState state = RetiringState::Free;
        std::uint64_t generation = 1;
        VnPackHandle handle = 0;
        PackRevisionKey revisionKey{};
        PackResourceRevisionToken resourceToken = 0;
        std::uint64_t closureRefCount = 0;
        AlignedBuffer bytes;
        std::vector<PackEntry> packEntries;
        std::vector<PCMRegistry::RetiredGeneration> entries;
        std::vector<PCMRegistry::RetiredGeneration> successorEntries;

        bool Drained() const {
            for (const PackEntry& entry : packEntries) {
                const PCMView view = entry.registration.View();
                if (view.useCount != nullptr &&
                    view.useCount->load(std::memory_order_acquire) != 0) {
                    return false;
                }
            }
            for (const PCMRegistry::RetiredGeneration& g : entries) {
                if (!g.Drained()) {
                    return false;
                }
            }
            return true;
        }
    };

    enum class ResourceClosureState : std::uint8_t {
        Free,
        Live,
        Quarantined,
    };

    struct ResourceClosureRow {
        ResourceClosureState state = ResourceClosureState::Free;
        std::uint32_t generation = 1;
        std::uint32_t resourceCount = 0;
        std::shared_ptr<const std::vector<PackResourceRevisionToken>> tokens;
    };

    Slot* SlotFor(VnPackHandle handle);
    const Slot* SlotFor(VnPackHandle handle) const;
    bool OtherPackReferences(VnPackHandle excluded, VnSoundId soundId,
                             std::uint64_t cookie) const noexcept;
    VnResult PrepareCanonicalMedia(
        VnPackHandle replacing, const AlignedBuffer& bytes,
        const std::vector<PackTocEntry>& parsedMedia,
        std::vector<PackEntry>& entries, PCMRegistry& registry,
        PCMRegistry::PreparedPackMutation* out);
    VnResult PrepareRetiringRow(
        VnPackHandle handle, std::size_t predecessorEntryCount,
        std::size_t successorEntryCount, bool preservePackEntries,
        RetiredReplace* outReservation,
        RetiringGeneration** outRow) noexcept;
    RetiringGeneration* RetiringRowFor(
        const RetiredReplace& reservation) noexcept;
    void ResetRetiringRow(RetiringGeneration& row) noexcept;
    void ReleaseRetiringRow(RetiringGeneration& row) noexcept;
    VnResult PrepareResourceRevisionToken(
        const PackRevisionKey* revisionKey, VnPackHandle expectedHandle,
        PackResourceRevisionToken* outToken) const noexcept;
    void CommitResourceRevisionToken(
        PackResourceRevisionToken token) noexcept;
    std::uint64_t* ResourceClosureRefCountFor(
        PackResourceRevisionToken resourceToken) noexcept;
    const ResourceClosureRow* ResourceClosureRowFor(
        PackResourceClosureToken closureToken) const noexcept;
    ResourceClosureRow* ResourceClosureRowFor(
        PackResourceClosureToken closureToken) noexcept;
    VnResult SelectResourceClosureRow(std::size_t* out) const noexcept;
    const PackResourceRevisionToken* ResourceClosureTokens(
        std::size_t rowIndex) const noexcept;

    // Shared read+validate front half of Load() and Replace(): reads the
    // whole file, proves every container invariant, and produces the parsed
    // TOC. Touches no slot or registry state.
    VnResult ReadAndValidate(const char* utf8Path, AlignedBuffer* outBytes,
                             std::vector<PackTocEntry>* outParsed) const;

    std::vector<Slot> m_slots;
    std::vector<RetiringGeneration> m_retiring;
    std::vector<ResourceClosureRow> m_resourceClosures;
    std::uint64_t m_staleHandleHits = 0;
    std::uint64_t m_loadedHighWater = 0;
    std::uint64_t m_noPackCount = 0;
    VnSoundId m_conflictingSoundId{};
    PackResourceRevisionToken m_nextResourceRevisionToken = 1;
    bool m_resourceRevisionTokensExhausted = false;
    bool m_pendingSetMutation = false;
};

} // namespace harken

#endif // HARKEN_MEDIA_PACK_TABLE_H
