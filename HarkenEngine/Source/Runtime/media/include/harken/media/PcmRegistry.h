// PCM registry: opaque VnSoundId -> immutable PCM. Registration runs on the
// logic thread and may allocate; after registration the data never changes
// and stays pinned until unregister. The render path reads registered data
// only through the PCMView captured at voice start — it never touches the
// map, so map mutations stay off the audio thread's read path.
//
// The registry is source-agnostic: keys are opaque ids and the registry
// neither knows nor cares where the bytes came from.
#ifndef HARKEN_MEDIA_PCM_REGISTRY_H
#define HARKEN_MEDIA_PCM_REGISTRY_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/PcmView.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace harken {

// Deterministic allocation failures for internal registration tests.
// Production leaves the fault at None.
enum class PcmRegisterFault : std::uint32_t {
    None = 0,
    SampleBacking = 1,
    MapCommit = 2
};

void SetPcmRegisterFaultForTests(PcmRegisterFault fault);
PcmRegisterFault PcmRegisterFaultForTests();

class PCMRegistry {
private:
    struct Entry {
        PCMView view;                     // useCount wired to `uses` below
        std::vector<float> ownedSamples;  // copy policy backing; empty for
                                          // reference policy
        std::atomic<std::uint32_t> uses{0};
        std::uint64_t cookie = 0;         // provenance stamp of this
                                          // registration (see RegisterPCM)
    };
    using Map = std::unordered_map<VnSoundId, std::shared_ptr<Entry>>;

public:
    class PackReference {
    public:
        bool Valid() const noexcept { return entry_ != nullptr; }
        PCMView View() const noexcept { return entry_ != nullptr ? entry_->view : PCMView{}; }
        std::uint64_t Cookie() const noexcept { return entry_ != nullptr ? entry_->cookie : 0; }

    private:
        friend class PCMRegistry;
        std::shared_ptr<const Entry> entry_;
    };

    struct PackReferenceRegistration {
        VnSoundId soundId{};
        VnPCMDesc desc{};
        PcmLoopRegion loop{};
    };

    class PreparedPackMutation {
    public:
        PreparedPackMutation() = default;
        PreparedPackMutation(const PreparedPackMutation&) = delete;
        PreparedPackMutation& operator=(const PreparedPackMutation&) = delete;
        PreparedPackMutation(PreparedPackMutation&&) noexcept = default;
        PreparedPackMutation& operator=(
            PreparedPackMutation&&) noexcept = default;

        bool IsPrepared() const noexcept { return m_owner != nullptr; }
        std::uint64_t CookieOf(VnSoundId soundId) const noexcept;
        PackReference ReferenceOf(VnSoundId soundId) const noexcept;

    private:
        friend class PCMRegistry;
        PCMRegistry* m_owner = nullptr;
        Map m_staged;
        std::uint64_t m_nextCookie = 1;
    };

    class PreparedReplaceToken {
    public:
        bool IsPrepared() const noexcept { return m_desc != nullptr; }
        const std::atomic<std::uint32_t>* GenerationIdentity() const noexcept {
            return m_generation;
        }

    private:
        friend class PCMRegistry;
        VnSoundId m_soundId{};
        const VnPCMDesc* m_desc = nullptr;
        std::uint64_t m_contentRevision = 0;
        PcmLoopRegion m_loop;
        const std::atomic<std::uint32_t>* m_generation = nullptr;
    };

    // One registration removed from the id map while logical voices may still
    // read it: a hot-reload old generation. The map node moves out whole, so
    // the entry's address — the sample views and the useCount atomic every
    // consuming voice holds — stays stable until the generation is destroyed.
    // Destruction frees copy-policy samples (logic thread); reference-policy
    // memory returns to its caller's ownership rules.
    class RetiredGeneration {
    public:
        RetiredGeneration() = default;
        RetiredGeneration(RetiredGeneration&&) = default;
        RetiredGeneration& operator=(RetiredGeneration&&) = default;

        // The id this generation was registered under before it retired.
        VnSoundId SoundId() const { return m_node.key(); }
        PCMView View() const { return m_node.mapped()->view; }
        std::uint64_t Cookie() const { return m_node.mapped()->cookie; }

        // True once no logical voice reads the generation: the shared use
        // count — incremented by live starts, pending-start pins, and park
        // pins alike — has reached zero.
        bool Drained() const {
            return m_node.mapped()->uses.load(std::memory_order_acquire) == 0;
        }

    private:
        friend class PCMRegistry;
        Map::node_type m_node;
    };

    // Validates the descriptor and stores the sound. Copy policy duplicates
    // the samples into registry-owned memory; reference policy records the
    // caller's pointers (caller keeps them pinned and unchanged until
    // unregister). An id that already exists is refused with VN_IN_USE.
    //
    // Every successful registration is stamped with a registry-unique,
    // monotonically increasing cookie, optionally returned through
    // outCookie. The cookie is provenance, not identity: a bulk owner
    // (the pack table) keeps it to recognize whether the entry
    // currently under an id is still the one it registered, since ids can
    // be unregistered and reused by anyone (the registry stays
    // source-agnostic). Internal surface only; callers that do not track
    // provenance ignore it.
    VnResult RegisterPCM(VnSoundId soundId, const VnPCMDesc* desc,
                         std::uint64_t* outCookie = nullptr,
                         PcmLoopRegion loop = {});

    // Removes a sound. Refused with VN_IN_USE while any voice plays it
    // (entry useCount != 0); VN_UNKNOWN_ID when the id was never registered.
    VnResult UnregisterPCM(VnSoundId soundId);

    // Hot reload: swaps the content under a registered id. The new descriptor
    // validates exactly as RegisterPCM's does (VN_INVALID_ARG); VN_UNKNOWN_ID
    // when the id names no registered PCM; VN_IN_USE while a previous replace
    // of this id still has a draining old generation (at most one retiring
    // generation per asset — the caller sweeps first so a drained one never
    // blocks). On success the old entry moves whole into the internal retired
    // list when voices still read it (destroyed immediately otherwise) and
    // the id resolves to the new content from this call on. All-or-nothing:
    // any refusal leaves the old registration fully in force.
    VnResult ReplacePCM(VnSoundId soundId, const VnPCMDesc* desc);
    VnResult PrepareReplacePCM(VnSoundId soundId, const VnPCMDesc* desc,
                               PreparedReplaceToken* out,
                               PcmLoopRegion loop = {}) const;
    VnResult CommitPreparedReplacePCM(PreparedReplaceToken* token);

    // Extracts the registration under soundId as a caller-owned retired
    // generation, regardless of its use count, provided the current
    // registration's provenance cookie matches (the pack-replace path: the
    // pack retires only entries that are still its own). False — extracting
    // nothing — when the id is absent or the cookie does not match.
    bool ExtractRetiring(VnSoundId soundId, std::uint64_t expectedCookie,
                         RetiredGeneration* out);

    VnResult PreparePackMutation(
        const PackReferenceRegistration* registrations,
        std::size_t registrationCount, std::size_t retiringCount,
        PreparedPackMutation* out);
    void CommitPreparedPackMutation(
        PreparedPackMutation* prepared) noexcept;

    // Reinserts an extracted generation under its original id (the pack
    // replace's throw-unwind path). The id must currently be vacant.
    void ReinsertRetired(RetiredGeneration&& generation);

    // Destroys every internally retired generation whose voices have drained.
    // Logic thread only (Vn_Update and the replace verbs call it).
    void SweepRetired();

    // Internally retired generations still pinned by voices (the PCM share of
    // the snapshot's pinnedGenerationCount).
    std::size_t RetiredCount() const { return m_retired.size(); }

    // Whether an internally retired generation for this id still exists.
    bool HasRetired(VnSoundId soundId) const;

    // Resolves an id to its immutable view (useCount pointer included).
    // Returns an invalid view when the id is unknown. The view's data
    // pointers stay valid until unregister; unordered_map guarantees entry
    // address stability across other insertions and erasures.
    PCMView Lookup(VnSoundId soundId) const;

    // Cookie of the registration currently holding the id, or 0 when the
    // id is unregistered (cookies start at 1, so 0 never matches).
    std::uint64_t CookieOf(VnSoundId soundId) const;

    std::size_t Count() const { return m_entries.size(); }

private:
    VnResult CommitPCM(VnSoundId soundId, const VnPCMDesc* desc,
                       std::vector<float>&& stagedSamples,
                       std::uint64_t contentRevision,
                       std::uint64_t* outCookie, PcmLoopRegion loop);

    friend class PackTable;
    bool m_pendingSetMutation = false;
    Map m_entries;
    // Old generations retired by ReplacePCM, each pinned until its voices
    // drain (SweepRetired destroys drained ones). Node extraction keeps every
    // entry address stable, so views captured before the replace stay valid.
    std::vector<RetiredGeneration> m_retired;
    std::uint64_t m_nextCookie = 1; // 0 reserved for "no registration"
};

} // namespace harken

#endif // HARKEN_MEDIA_PCM_REGISTRY_H
