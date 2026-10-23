#ifndef HARKEN_CORE_PACK_COMPOSITION_H
#define HARKEN_CORE_PACK_COMPOSITION_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/PackPlaybackView.h"
#include "harken/core/PackReadinessContract.h"
#include "harken/core/PackCompanionNames.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/core/StableRowStorage.h"
#include "harken/media/PackLogicImage.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace harken {

enum class PackAuthorityMode : std::uint32_t {
    Unspecified = 0,
    Pack = 1,
    DirectWorld = 2,
};

enum class PackOwnerReadiness : std::uint32_t {
    Inactive = 0,
    Preparing = 1,
    Active = 2,
    Retiring = 3,
};

struct PackBindingManifestV1 {
    std::vector<VnBindingId> definitionIds;
    std::vector<PackBindingAttachmentEntry> attachments;
};

struct PackOwnerLocalFacts {
    std::uint64_t packStableId = 0;
    VnPackHandle handle = 0;
    std::uint64_t contentRevision = 0;
    std::uint32_t slotGeneration = 0;
    std::uint64_t partitionRevision = 0;
    std::uint64_t resourceRevision = 0;
    std::uint64_t compositeGeneration = 0;
    PackOwnerReadiness readiness = PackOwnerReadiness::Inactive;
    std::uint64_t catalogCount = 0;
    std::uint64_t catalogBytes = 0;
    std::uint64_t loadedCount = 0;
    std::uint64_t loadedBytes = 0;
    std::uint64_t highWaterLoadedCount = 0;
    std::uint64_t highWaterLoadedBytes = 0;
    std::uint64_t lastRefusalRequired = 0;
    std::uint64_t lastRefusalConfigured = 0;
    VnResult lastRefusalResult = VN_OK;
    std::uint64_t activationFrame = 0;
    std::uint64_t replacementFrame = 0;
    std::uint64_t retirementFrame = 0;
    std::uint64_t releaseFrame = 0;
    VnObjectId conflictingStableId{};
    VnObjectId conflictingProjectId{};
    std::uint64_t conflictingPackA = 0;
    std::uint64_t conflictingPackB = 0;
};

struct Engine;
class EngineOriginalPackIdentityLease;

class PackComposition {
public:
    enum class OriginalLeasePhase : std::uint8_t {
        Unissued, Preparing, Live, DefinitionEnded, Released, Faulted, QuiescedDisposed
    };
    class OriginalLeaseEpisode {
    public:
        OriginalLeaseEpisode() noexcept = default;
        ~OriginalLeaseEpisode();
        OriginalLeaseEpisode(const OriginalLeaseEpisode&) = delete;
        OriginalLeaseEpisode& operator=(const OriginalLeaseEpisode&) = delete;
        OriginalLeaseEpisode(OriginalLeaseEpisode&& other) noexcept;
        OriginalLeaseEpisode& operator=(OriginalLeaseEpisode&& other) noexcept;
        bool Valid() const noexcept;
        OriginalLeasePhase Phase() const noexcept;
        PackPartitionLease OriginalLease() const noexcept;
    private:
        friend class PackComposition;
        friend struct Engine;
        friend class EngineOriginalPackIdentityLease;
        const PackComposition* owner_ = nullptr;
        std::size_t index_ = SIZE_MAX;
        std::uint64_t token_ = 0;
        std::uint64_t incarnation_ = 0;
        OriginalLeasePhase lastPhase_ = OriginalLeasePhase::Unissued;
        void Detach() noexcept;
        void MoveFrom(OriginalLeaseEpisode& other) noexcept;
    };
    ~PackComposition();
    PackComposition(const PackComposition&) = delete;
    PackComposition& operator=(const PackComposition&) = delete;
    PackComposition(PackComposition&&) = delete;
    PackComposition& operator=(PackComposition&&) = delete;
    VnResult AbortOriginalPreparedLease(OriginalLeaseEpisode& original) const noexcept;
    bool HasOriginalLeaseEpisode(const PackPartitionLease& lease) const noexcept;
    struct PublishedOwner {
        std::uint64_t packStableId = 0;
        std::uint64_t contentHash = 0;
        std::shared_ptr<const PackCompanionNames> companionNames;
        VnPackHandle handle = 0;
        std::uint64_t contentRevision = 0;
        std::uint64_t partitionRevision = 0;
        std::uint64_t resourceRevision = 0;
        PackResourceRevisionToken resourceToken = 0;
        std::uint32_t slotIndex = 0;
        PackOwnerReadiness readiness = PackOwnerReadiness::Inactive;
        std::shared_ptr<CurrentWorldBodyImage> body;
        ContainerWorldInstallViewV1 view{};
        std::shared_ptr<const std::vector<PackVoiceBinding>> voiceBindings;
        std::uint64_t definitionBytes = 0;
        std::vector<PackDependencyEdge> dependencyEdges;
        std::uint64_t providerClosureFingerprint = 0;
        bool hasGameplayCatalog = false;
        GameplayCatalogImage gameplayCatalog;
        bool hasVariableNameCatalog = false;
        PackVariableNameCatalog variableNameCatalog;
        std::vector<PackCanonicalImage::VariablePolicyOverlay> variablePolicyOverlays;
        bool hasVariablePolicyOverlay = false;
        std::vector<PackCanonicalImage::VariableRequirement> variableRequirements;
        bool hasVariableRequirements = false;
        bool hasBusDefinitionSnapshot = false;
        PackBusDefinitionSnapshotHeaderV1 busDefinitionSnapshotHeader{};
        CanonicalBusTriple requirementTriple{};
        CanonicalBusTriple partitionTriple{};
        PackBindingManifestV1 bindingManifest;
    };

    struct IdentityIndexEntry {
        VnObjectId stableId{};
        std::uint32_t packSlot = 0;
        std::uint32_t localRow = 0;
        std::uint32_t identityClass = 0;
    };

    struct CanonicalPackSetLedgerV1 {
        struct AcceptedMember {
            std::uint64_t stableId = 0;
            std::uint64_t hash = 0;
            std::uint32_t flags = 0;
            std::uint32_t kind = 0;
            std::string path;
            std::string root;
        };
        std::uint64_t setRevision = 0;
        std::uint64_t activationGeneration = 0;
        std::uint64_t semanticKey = 0;
        std::uint64_t busContentGeneration = 0;
        std::uint64_t publishedBusFingerprint = 0;
        std::uint64_t compileTargetProfileFingerprint = 0;
        std::uint64_t masterFingerprint = 0;
        VnPackHandle initHandle = 0;
        std::uint32_t memberCount = 0;
        std::uint8_t packMasterHeld = 0;
        std::uint8_t reserved0 = 0;
        std::uint16_t reserved1 = 0;
        std::vector<std::uint8_t> lastReport;
        std::vector<std::uint8_t> lastMemberResults;
        std::vector<AcceptedMember> acceptedMembers;
    };

    struct PublishedView {
        struct ActionName {
            std::string name;
            VnObjectId actionId{};
            std::uint64_t ownerPackId = 0;
            std::uint64_t fingerprint = 0;
        };
        VnObjectId projectId{};
        bool companionNames = false;
        std::vector<ActionName> actionNames;
        std::uint64_t compositeGeneration = 0;
        std::vector<PublishedOwner> owners;
        std::vector<IdentityIndexEntry> index;
        PackCanonicalImage catalog;
        bool hasCatalog = false;
        std::uint64_t initPackStableId = 0;
        VnPackHandle initHandle = 0;
        bool hasGameplayCatalog = false;
        GameplayCatalogImage gameplayCatalog;
        bool hasVariableNameCatalog = false;
        PackVariableNameCatalog variableNameCatalog;
        std::shared_ptr<const PackPlaybackOwnerStore> playbackStore;
        CanonicalPackSetLedgerV1 ledger{};
        PackAttenuationBindingMapV1 attenuationBindings;
    };

    struct CandidateOwner {
        std::uint64_t packStableId = 0;
        std::uint64_t contentHash = 0;
        std::shared_ptr<const PackCompanionNames> companionNames;
        VnPackHandle handle = 0;
        std::uint64_t contentRevision = 0;
        std::uint64_t partitionRevision = 0;
        std::uint64_t resourceRevision = 0;
        PackResourceRevisionToken resourceToken = 0;
        std::uint32_t slotIndex = 0;
        PackOwnerReadiness readiness = PackOwnerReadiness::Active;
        std::vector<std::uint8_t> partitionBody;
        std::uint32_t partitionBodyVintage = kCurrentWorldShippingBodyVintage;
        bool hasPartition = false;
        std::shared_ptr<CurrentWorldBodyImage> body;
        ContainerWorldInstallViewV1 view{};
        std::shared_ptr<const std::vector<PackVoiceBinding>> voiceBindings;
        std::uint64_t definitionBytes = 0;
        std::vector<PackDependencyEdge> dependencyEdges;
        bool hasGameplayCatalog = false;
        GameplayCatalogImage gameplayCatalog;
        bool hasVariableNameCatalog = false;
        PackVariableNameCatalog variableNameCatalog;
        std::vector<PackCanonicalImage::VariablePolicyOverlay> variablePolicyOverlays;
        bool hasVariablePolicyOverlay = false;
        std::vector<PackCanonicalImage::VariableRequirement> variableRequirements;
        bool hasVariableRequirements = false;
        bool hasBusDefinitionSnapshot = false;
        PackBusDefinitionSnapshotHeaderV1 busDefinitionSnapshotHeader{};
        CanonicalBusTriple requirementTriple{};
        CanonicalBusTriple partitionTriple{};
        PackBindingManifestV1 bindingManifest;
    };

    class PreparedCommit {
    public:
        PreparedCommit() = default;
        PreparedCommit(const PreparedCommit&) = delete;
        PreparedCommit& operator=(const PreparedCommit&) = delete;
        PreparedCommit(PreparedCommit&&) noexcept = default;
        PreparedCommit& operator=(PreparedCommit&&) noexcept = default;

        bool IsPrepared() const noexcept { return owner_ != nullptr; }
        bool NamesOnly() const noexcept { return namesOnly_; }
        const PublishedView* Published() const noexcept {
            return published_.get();
        }

    private:
        friend class PackComposition;
        PackComposition* owner_ = nullptr;
        std::shared_ptr<PublishedView> published_;
        PackOwnerLocalFacts facts_{};
        bool namesOnly_ = false;
    };

    static CandidateOwner Keep(const PublishedOwner& existing) {
        CandidateOwner keep;
        keep.packStableId = existing.packStableId;
        keep.contentHash = existing.contentHash;
        keep.companionNames = existing.companionNames;
        keep.handle = existing.handle;
        keep.contentRevision = existing.contentRevision;
        keep.partitionRevision = existing.partitionRevision;
        keep.resourceRevision = existing.resourceRevision;
        keep.resourceToken = existing.resourceToken;
        keep.slotIndex = existing.slotIndex;
        keep.readiness = PackOwnerReadiness::Active;
        keep.body = existing.body;
        keep.view = existing.view;
        keep.partitionBodyVintage = existing.view.sourceBodyVintage;
        keep.voiceBindings = existing.voiceBindings;
        keep.definitionBytes = existing.definitionBytes;
        keep.hasPartition = existing.body != nullptr;
        keep.dependencyEdges = existing.dependencyEdges;
        keep.hasGameplayCatalog = existing.hasGameplayCatalog;
        keep.gameplayCatalog = existing.gameplayCatalog;
        keep.hasVariableNameCatalog = existing.hasVariableNameCatalog;
        keep.variableNameCatalog = existing.variableNameCatalog;
        keep.variablePolicyOverlays = existing.variablePolicyOverlays;
        keep.hasVariablePolicyOverlay = existing.hasVariablePolicyOverlay;
        keep.variableRequirements = existing.variableRequirements;
        keep.hasVariableRequirements = existing.hasVariableRequirements;
        keep.hasBusDefinitionSnapshot = existing.hasBusDefinitionSnapshot;
        keep.busDefinitionSnapshotHeader = existing.busDefinitionSnapshotHeader;
        keep.requirementTriple = existing.requirementTriple;
        keep.partitionTriple = existing.partitionTriple;
        keep.bindingManifest = existing.bindingManifest;
        return keep;
    }

    explicit PackComposition(std::size_t leaseCapacity)
        : PackComposition(leaseCapacity, 0, 32) {}
    PackComposition(std::size_t sharedActionDeferredCapacity,
                    std::size_t playableCapacity, std::size_t maxPacks,
                    std::uint64_t generationFloor = 0);

    PackAuthorityMode Authority() const noexcept { return authority_; }
    std::uint64_t CompositeGeneration() const noexcept {
        return published_ != nullptr ? published_->compositeGeneration : 0;
    }
    const PublishedView* Published() const noexcept { return published_.get(); }
    std::shared_ptr<const PublishedView> PublishedHandle() const noexcept {
        return published_;
    }
    const PackOwnerLocalFacts& Facts() const noexcept { return facts_; }

    VnResult EnterPackAuthority() noexcept;
    VnResult EnterDirectWorldAuthority() noexcept;

    VnResult PrepareCandidate(const PackCanonicalImage& image,
                              VnPackHandle handle,
                              CandidateOwner* out,
                              PackResourceRevisionToken resourceToken = 0) const;
    VnResult Commit(std::vector<CandidateOwner> owners,
                    const PackCanonicalImage* catalogOrNull,
                    VnPackHandle initHandle,
                    std::uint32_t maxActions,
                    std::uint64_t serviceFrame);
    VnResult PrepareCommit(std::vector<CandidateOwner> owners,
                           const PackCanonicalImage* catalogOrNull,
                           VnPackHandle initHandle,
                           std::uint32_t maxActions,
                           std::uint64_t serviceFrame,
                           PreparedCommit* out,
                           const PackAttenuationBindingMapV1* attenuationBindings = nullptr,
                           const CanonicalPackSetLedgerV1* ledger = nullptr,
                           bool clearInit = false,
                           const GameplayCatalogImage* gameplayCatalog = nullptr);
    void CommitPrepared(PreparedCommit* prepared) noexcept;
    void RestorePublished(std::shared_ptr<const PublishedView> previous) noexcept;
    VnResult ValidateLease(const PackPartitionLease& lease) const noexcept;
    VnResult ReleaseLease(const PackPartitionLease& lease) noexcept;
    VnResult RetagLiveLease(const PackPartitionLease& lease,
                            PackLeaseSlice to,
                            const OriginalLeaseEpisode* original = nullptr) noexcept;
    std::uint64_t RetagBackpressureCountForTests() const noexcept {
        return retagBackpressureCountForTests_;
    }
    VnResult SetNextLeaseTokenForTests(std::uint64_t nextToken) noexcept;
    void SetLeaseStorageCommitPageLimitsForTests(std::size_t leases,
        std::size_t originals, std::size_t closures) noexcept {
        leases_.SetCommitPageLimitForTests(leases);
        originalLeaseRecords_.SetCommitPageLimitForTests(originals);
        frozenClosures_.SetCommitPageLimitForTests(closures);
    }
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    mutable bool refuseOriginalLeaseCommitOnceForTest = false;
#endif
    VnResult PrepareLookup(VnObjectId stableId,
                           PackIdentityLookup* out) const noexcept;
    VnResult PrepareLookup(VnObjectId stableId, PackLeaseSlice slice,
                           PackIdentityLookup* out,
                           OriginalLeaseEpisode* original = nullptr) const noexcept;
    VnResult BuildResourceClosure(
        const PackPartitionLease& lease,
        PackResourceRevisionToken* outTokens, std::size_t tokenCapacity,
        std::size_t* outTokenCount) const noexcept;
    VnResult PreparedResourceClosureBacking(const PackPartitionLease& lease,
        std::shared_ptr<const std::vector<PackResourceRevisionToken>>* out) const noexcept;
    VnResult CommitPreparedLease(
        PackPartitionLease* lease,
        PackResourceClosureToken resourceClosureToken,
        const OriginalLeaseEpisode* original = nullptr) const noexcept;
    void AbandonPreparedLease(
        const PackPartitionLease& lease) const noexcept;

    PackIdentityLookup Lookup(VnObjectId stableId) const noexcept;
    ActionProgramHandleV1 FindActionAtLease(
        const PackPartitionLease& lease,
        VnObjectId actionId) const noexcept;
    const ContainerProgramViewV3* FindContainerAtLease(
        const PackPartitionLease& lease,
        VnObjectId containerStableId) const noexcept;
    bool PlayableWorldAtLease(const PackPartitionLease& lease,
                              PlayableWorldViewV1* out) const noexcept;
    bool PlaybackViewAtLease(const PackPartitionLease& lease,
                             PackPlaybackView* out) const noexcept;
    VnObjectId ProjectIdAtLease(const PackPartitionLease& lease) const noexcept;
    ActionProgramHandleV1 FindActionIn(
        const PublishedView& view, VnObjectId actionId) const noexcept;
    ActionProgramHandleV1 ResolveAction(VnObjectId actionId) const noexcept;
    const ActionProgramViewV1* FindAction(VnObjectId actionId) const noexcept;
    const ContainerProgramViewV3* FindContainer(
        VnObjectId containerStableId) const noexcept;
    bool RequiredConsumerExists(std::uint64_t providerStableId,
                                std::uint64_t* outConsumer) const noexcept;
    bool HasLiveLease(std::uint64_t packStableId,
                      std::uint64_t partitionRevision,
                      std::uint64_t resourceRevision) const noexcept;
    bool HasLiveResourceToken(
        PackResourceRevisionToken resourceToken) const noexcept;
    bool HasLiveAttenuationGeneration(VnAttenuationId id,
                                      std::uint64_t generation) const noexcept;
    void NoteRefusal(std::uint64_t required, std::uint64_t configured,
                     VnResult result) noexcept;

private:
    friend struct Engine;
    struct OriginalLeaseRecord {
        bool active = false;
        OriginalLeasePhase phase = OriginalLeasePhase::Unissued;
        PackLeaseSlice tag = PackLeaseSlice::SharedActionAndDeferred;
        PackPartitionLease lease{};
        OriginalLeaseEpisode* handle = nullptr;
        void* resourceReceiver = nullptr;
        PackResourceClosureToken acquiredResource = 0;
        bool resourceReturned = false;
        bool definitionEnded = false;
        void* originalPreparingStorage = nullptr;
        std::uint64_t originalPreparingGeneration = 0;
        void* originalInvocation = nullptr;
        std::uint64_t originalInvocationGeneration = 0;
        std::uint32_t originalInvocationSlot = UINT32_MAX;
        bool originalInvocationEnded = false;
        bool originalInvocationQuiesced = false;
        VnResult resourceResult = VN_OK;
        VnResult definitionResult = VN_OK;
    };
    bool MatchesOriginalLease(const OriginalLeaseEpisode& original) const noexcept;
    static bool SameFullOriginalLease(const PackPartitionLease& a, const PackPartitionLease& b) noexcept;
    bool BindOriginalResources(OriginalLeaseEpisode& original, void* receiver,
        PackResourceClosureToken resource) const noexcept;
    VnResult EndOriginalDefinition(std::size_t index) const noexcept;
    VnResult DischargeOriginalLease(OriginalLeaseEpisode& original) const noexcept;
    void QuiesceOriginalLease(std::size_t index) const noexcept;
    enum class LeaseState : std::uint8_t {
        Free,
        Prepared,
        Live,
    };

    struct LeaseRow {
        LeaseState state = LeaseState::Free;
        PackLeaseSlice tag = PackLeaseSlice::SharedActionAndDeferred;
        PackPartitionLease lease{};
        std::uint32_t refs = 0;
    };

    static bool SameIdentity(const IdentityIndexEntry& a,
                             const IdentityIndexEntry& b) noexcept;
    static bool SameLease(const PackPartitionLease& a,
                          const PackPartitionLease& b) noexcept;
    static bool SameLeaseIdentity(const PackPartitionLease& a,
                                  const PackPartitionLease& b) noexcept;
    VnResult DecodeOwner(CandidateOwner& owner) const;
    VnResult BuildIndex(PublishedView& view,
                        PackOwnerLocalFacts* facts) const;
    VnResult PrepareLease(const PublishedOwner& owner, PackLeaseSlice slice,
                          PackPartitionLease* out,
                          OriginalLeaseEpisode* original) const noexcept;
    bool PrepareLeaseStorage() const noexcept;
    std::size_t LeaseIndex(const LeaseRow& row) const noexcept;
    PackFrozenClosure& FrozenClosure(LeaseRow& row) const noexcept;
    const PackFrozenClosure& FrozenClosure(const LeaseRow& row) const noexcept;
    const ContainerWorldInstallViewV1* DefinitionView(const LeaseRow& row) const noexcept;
    void ClearLeaseRow(LeaseRow& row) const noexcept;
    VnResult SnapshotFrozenClosure(const PublishedOwner& root,
                                   LeaseRow& row) const noexcept;
    std::size_t SliceCapacity(PackLeaseSlice slice) const noexcept;
    std::size_t& SliceOccupied(PackLeaseSlice slice) const noexcept;

    PackAuthorityMode authority_ = PackAuthorityMode::Unspecified;
    std::shared_ptr<const PublishedView> published_;
    std::size_t sharedCapacity_ = 0;
    std::size_t playableCapacity_ = 0;
    std::size_t maxPacks_ = 0;
    std::uint64_t generationFloor_ = 0;
    mutable std::size_t sharedOccupied_ = 0;
    mutable std::size_t playableOccupied_ = 0;
    mutable StableRowStorage<LeaseRow, std::size_t> leases_;
    mutable StableRowStorage<OriginalLeaseRecord, std::size_t> originalLeaseRecords_;
    std::uint64_t originalLeaseIncarnation_ = 0;
    mutable bool originalLeasesQuiesced_ = false;
    mutable StableRowStorage<PackFrozenClosure, std::size_t> frozenClosures_;
    mutable std::uint64_t nextLeaseToken_ = 1;
    std::uint64_t retagBackpressureCountForTests_ = 0;
    mutable bool leaseTokensExhausted_ = false;
    mutable PackOwnerLocalFacts facts_{};
};

} // namespace harken

#endif
