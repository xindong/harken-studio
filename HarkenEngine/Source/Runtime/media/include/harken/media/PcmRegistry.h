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
#include "harken/core/StableRowStorage.h"
#include "harken/media/PackMediaRange.h"
#include "harken/core/MediaCodec.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace harken {

namespace detail {

struct PcmSampleDeleter {
    PcmSampleDeleter() noexcept = default;
    PcmSampleDeleter(const PcmSampleDeleter&) = delete;
    PcmSampleDeleter& operator=(const PcmSampleDeleter&) = delete;
    PcmSampleDeleter(PcmSampleDeleter&&) noexcept = default;
    PcmSampleDeleter& operator=(PcmSampleDeleter&&) noexcept = default;
    void operator()(void* p) noexcept;
    PcmSampleFormat format = PcmSampleFormat::Float32;
    std::unique_ptr<StablePageReservation> pages;
};

using PcmSampleBuffer = std::unique_ptr<void, PcmSampleDeleter>;

} // namespace detail

// Deterministic allocation failures for internal registration tests.
// Production leaves the fault at None.
enum class PcmRegisterFault : std::uint32_t {
    None = 0,
    SampleBacking = 1,
    MapCommit = 2
};

void SetPcmRegisterFaultForTests(PcmRegisterFault fault);
PcmRegisterFault PcmRegisterFaultForTests();

// Semantic facts of a registered sound, independent of which storage
// representations (resident planes, Pack range) back it.
struct PcmMediaFacts {
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::uint64_t frames = 0;
    PcmLoopRegion loop;
    std::uint64_t contentRevision = 0;
    bool Valid() const noexcept { return channelCount > 0 && frames > 0; }
};

class VorbisCodec;

class PCMRegistry {
private:
    struct Entry {
        PCMView view;                     // useCount wired to `uses` below;
                                          // invalid when only a range backs it
        detail::PcmSampleBuffer ownedSamples; // copy policy backing; null for
                                                // reference policy
        // Optional immutable Pack range. Resident storage may be added once,
        // at a transaction commit, to an entry backed only by this range.
        std::shared_ptr<const PackMediaRange> range;
        std::shared_ptr<const EncodedMedia> encoded;
        std::atomic<std::uint32_t> uses{0};
        std::uint64_t cookie = 0;         // provenance stamp of this
                                          // registration (see RegisterPCM)

        PcmMediaFacts Facts() const noexcept;
    };
    using Map = std::unordered_map<VnSoundId, std::shared_ptr<Entry>>;

public:
    class PackReference {
    public:
        bool Valid() const noexcept { return entry_ != nullptr; }
        // True when the backing does not borrow a Pack byte buffer.
        bool OwnsSamples() const noexcept {
            return entry_ != nullptr &&
                   (entry_->ownedSamples != nullptr || entry_->range != nullptr || entry_->encoded != nullptr);
        }
        PCMView View() const noexcept { return entry_ != nullptr ? entry_->view : PCMView{}; }
        PcmMediaFacts Facts() const noexcept {
            return entry_ != nullptr ? entry_->Facts() : PcmMediaFacts{};
        }
        std::shared_ptr<const PackMediaRange> Range() const noexcept {
            return entry_ != nullptr ? entry_->range : nullptr;
        }
        std::uint64_t Cookie() const noexcept { return entry_ != nullptr ? entry_->cookie : 0; }
        bool SharesEntry(const PackReference& other) const noexcept { return entry_ == other.entry_; }

    private:
        friend class PCMRegistry;
        std::shared_ptr<const Entry> entry_;
    };

    class PackSampleBuilder;

    // Only registry preparation can produce an immutable, owned sample record.
    // Its entry stays at the same address when a pack transaction adopts it.
    class PreparedPackSamples {
    public:
        PreparedPackSamples() = default;
        PreparedPackSamples(const PreparedPackSamples&) = delete;
        PreparedPackSamples& operator=(const PreparedPackSamples&) = delete;
        PreparedPackSamples(PreparedPackSamples&&) noexcept = default;
        PreparedPackSamples& operator=(PreparedPackSamples&&) noexcept = default;

        bool IsPrepared() const noexcept { return m_entry != nullptr; }
        VnSoundId SoundId() const noexcept { return m_soundId; }
        PCMView View() const noexcept { return m_entry != nullptr ? m_entry->view : PCMView{}; }
        PcmMediaFacts Facts() const noexcept {
            return m_entry != nullptr ? m_entry->Facts() : PcmMediaFacts{};
        }
        const PackMediaRange* Range() const noexcept {
            return m_entry != nullptr ? m_entry->range.get() : nullptr;
        }
        bool SharesEntry(const PreparedPackSamples& other) const noexcept {
            return m_entry != nullptr && m_entry == other.m_entry;
        }

    private:
        friend class PCMRegistry;
        friend class PackSampleBuilder;
        VnSoundId m_soundId{};
        std::shared_ptr<Entry> m_entry;
    };

    // Accepts samples in the descriptor's physical order. Only Complete can
    // publish a record, after all frames and the canonical revision validate.
    class PackSampleBuilder {
    public:
        PackSampleBuilder() = default;
        PackSampleBuilder(const PackSampleBuilder&) = delete;
        PackSampleBuilder& operator=(const PackSampleBuilder&) = delete;
        PackSampleBuilder(PackSampleBuilder&&) noexcept = default;
        PackSampleBuilder& operator=(PackSampleBuilder&&) noexcept = default;
        VnResult Append(const float* samples, std::size_t count);
        VnResult Complete(PreparedPackSamples* out);
        // False preserves both builders and outputs for their single-record path.
        bool TryCompletePair(PackSampleBuilder& following,
            PreparedPackSamples* firstOut, PreparedPackSamples* followingOut,
            VnResult (&results)[2]);
        bool ReservationUnavailable() const noexcept { return m_reservationUnavailable; }
        void SetCommitPageLimitForTests(std::size_t pages) noexcept {
            if (m_entry != nullptr && m_entry->ownedSamples.get_deleter().pages != nullptr)
                m_entry->ownedSamples.get_deleter().pages->SetCommitPageLimitForTests(pages);
        }

    private:
        friend class PCMRegistry;
        VnResult PrepareStorage();
        VnResult Store(const float* samples, std::size_t count);
        void PrepareSampleView() noexcept;
        void MoveCompleted(PreparedPackSamples* out) noexcept;
        VnSoundId m_soundId{};
        std::shared_ptr<Entry> m_entry;
        std::shared_ptr<Entry> m_equalCandidate;
        std::size_t m_count = 0;
        std::size_t m_written = 0;
        VnResult m_status = VN_OK;
        bool m_reservationUnavailable = false;
    };

    // Resident storage built privately from a range-only entry's own range.
    // Commit installs it into that entry; dropping it changes nothing.
    class PreparedResidentUpgrade {
    public:
        PreparedResidentUpgrade() = default;
        PreparedResidentUpgrade(const PreparedResidentUpgrade&) = delete;
        PreparedResidentUpgrade& operator=(const PreparedResidentUpgrade&) = delete;
        PreparedResidentUpgrade(PreparedResidentUpgrade&&) noexcept = default;
        PreparedResidentUpgrade& operator=(PreparedResidentUpgrade&&) noexcept = default;
        bool IsPrepared() const noexcept { return m_target != nullptr; }
        VnSoundId SoundId() const noexcept { return m_soundId; }

    private:
        friend class PCMRegistry;
        VnSoundId m_soundId{};
        std::shared_ptr<Entry> m_target;
        detail::PcmSampleBuffer m_samples;
    };

    struct PackReferenceRegistration {
        VnSoundId soundId{};
        VnPCMDesc desc{};
        PcmLoopRegion loop{};
        PreparedPackSamples* samples = nullptr;
        // Reuse semantic identity without conflating per-Pack byte sources.
        PackReference identity;
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
        static PackReference AdoptExternalAlias(PreparedPackSamples& samples,
            const PackReference& identity) noexcept;

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
        PackReference Reference() const { return MakeReference(m_node.mapped()); }
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

    // Copy-policy input only. A nonempty output is refused without changing it.
    VnResult PreparePackSamples(VnSoundId soundId, const VnPCMDesc* desc,
                               PreparedPackSamples* out, PcmLoopRegion loop = {});
    // Shape validation does not access the descriptor's sample pointers.
    VnResult BeginPackSamples(VnSoundId soundId, const VnPCMDesc* shape,
                             PackSampleBuilder* out, PcmLoopRegion loop = {},
                             const PreparedPackSamples* equalCandidate = nullptr);

    VnResult PrepareVorbisSamples(VnSoundId soundId, VorbisCodec& codec,
        std::vector<std::uint8_t> bytes, std::size_t budget, PreparedPackSamples* out);

    // Range-only record. A caller that has proven bit-exact equality with an
    // earlier prepared record of the same id passes it to share that entry.
    VnResult PreparePackRangeSamples(VnSoundId soundId,
        std::shared_ptr<const PackMediaRange> range, PreparedPackSamples* out,
        const PreparedPackSamples* provenEqual = nullptr);

    // Reads and verifies a range-only entry into private resident storage.
    // Refuses an entry that already has resident samples or has no range.
    VnResult PrepareResidentUpgrade(const PackReference& reference,
                                    PreparedResidentUpgrade* out);
    // Logic thread. Render readers capture PCMView by value, so publishing
    // the planes here cannot race a reader of the entry.
    void CommitResidentUpgrade(PreparedResidentUpgrade* prepared) noexcept;

    // A completed record is consumed when its staged map entry is inserted.
    // Later refusal destroys consumed entries; unconsumed records stay owned
    // by the caller. Publication and cookie advancement occur only at commit.
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

    // Capture the exact current registration without allocating. A Pack keeps
    // this reference to track readers of its backing after an independent replace.
    PackReference ReferenceOf(VnSoundId soundId,
                              std::uint64_t expectedCookie) const noexcept;

    std::size_t Count() const { return m_entries.size(); }

private:
    static PackReference MakeReference(std::shared_ptr<const Entry> entry) noexcept {
        PackReference reference;
        reference.entry_ = std::move(entry);
        return reference;
    }
    VnResult CommitPCM(VnSoundId soundId, const VnPCMDesc* desc,
                       detail::PcmSampleBuffer&& stagedSamples,
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
