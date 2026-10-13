#ifndef HARKEN_API_PROJECT_GENERATION_TRANSACTION_H
#define HARKEN_API_PROJECT_GENERATION_TRANSACTION_H

#include <harken/ObjectId.h>
#include "PlayableRuntime.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/EngineConfig.h"
#include "harken/param/VariableService.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace harken {

struct Engine;
struct ProjectGenerationReservationDomainV1;

struct BusTopologyOwnerRecord {
    bool live = false;
    std::uint64_t ownerId = 0;
    std::uint64_t currentRevision = 0;
    std::uint64_t predecessorRevision = 0;
    std::uint64_t digestLow = 0;
    std::uint64_t digestHigh = 0;
};

struct PreviewMasterHolderV1 {
    bool occupied = false;
    std::uint64_t ownerId = 0;
    std::uint64_t revision = 0;
    std::uint64_t fingerprint = 0;
};

enum class TopologyOwnerRevisionDispositionV1 : std::uint8_t { Conflict, Retry, Advance };
TopologyOwnerRevisionDispositionV1 ClassifyTopologyOwnerRevisionV1(
    const BusTopologyOwnerRecord* current, std::uint64_t baseRevision,
    std::uint64_t revision, std::uint64_t digestLow, std::uint64_t digestHigh) noexcept;
bool MasterHasAuthoredClaimV1(const Engine& engine) noexcept;
bool PreviewOwnsSoleMasterV1(const Engine& engine, std::uint64_t ownerId) noexcept;
std::uint64_t NeutralMasterFingerprintV1(const Engine& engine) noexcept;
bool PreviewMasterCompatible(const Engine& engine, std::uint64_t fingerprint) noexcept;
std::uint32_t SelectPreviewOwnerDestination(const Engine& engine, std::uint64_t ownerId) noexcept;
bool HasPredecessorDirectWorldPublication(const Engine& engine) noexcept;
inline constexpr std::uint64_t kCanonicalPackSetMasterOwnerId = 0x5053434E00000001ull;
bool CanResetNeutralBusPropertiesV1(const Engine& engine, std::uint32_t slot) noexcept;
void ResetNeutralBusPropertiesV1(Engine& engine, std::uint32_t slot) noexcept;
bool CanResetVolumeOnlyNonVolumePropertiesV2(
    const Engine& engine, std::uint32_t slot,
    std::uint64_t allowedOwnerCookie = 0) noexcept;
void ResetVolumeOnlyNonVolumePropertiesV2(Engine& engine,
                                          std::uint32_t slot) noexcept;
struct TopologyPlanView;
bool ValidateTopologyPlanVolumesV1(const TopologyPlanView& plan) noexcept;
struct BusSlotRef;
bool CanRetireTopologyBusV1(const Engine& engine, BusSlotRef ref, std::uint64_t revision) noexcept;
void CommitRetireTopologyBusV1(Engine& engine, BusSlotRef ref, std::uint64_t revision) noexcept;
void ClearRetiredBusPropertiesV1(Engine& engine, BusSlotRef ref) noexcept;
bool HasTopologyBusConsumersV1(Engine& engine, BusSlotRef ref) noexcept;
std::uint32_t DrainTopologyRetirementsV1(Engine& engine) noexcept;
void CommitMigratedLogicVoiceMirrorsV1(Engine& engine, const std::uint32_t* migrationTargets,
    const std::uint32_t* generations, const std::uint8_t* alive, std::uint32_t slotCount) noexcept;
struct EnginePreparedTimelineVoiceStart;
struct EnginePreparedRoutingMigrationV1;
bool ValidatePreparedTimelineVoiceStartRoutingV1(Engine& engine,
    const EnginePreparedTimelineVoiceStart& prepared) noexcept;
bool PrepareUnpublishedTimelineRoutingV1(Engine& engine, const TopologyPlanView& candidate,
    EnginePreparedRoutingMigrationV1& out);
bool CanCommitUnpublishedTimelineRoutingV1(Engine& engine, const TopologyPlanView& candidate,
    const EnginePreparedRoutingMigrationV1& prepared) noexcept;
void CommitUnpublishedTimelineRoutingV1(Engine& engine,
    EnginePreparedRoutingMigrationV1& prepared) noexcept;

struct ProspectiveTopologyView {
    std::uint32_t busCount = 0;
    const VnBusId* busIds = nullptr;
    const std::uint32_t* generations = nullptr;
    const std::uint32_t* parents = nullptr;
    const std::uint8_t* alive = nullptr;
    const std::uint32_t* sendTargets = nullptr;
    const float* sendLevels = nullptr;
    const std::uint8_t* sendLevelApply = nullptr;
    std::uint32_t* feederCount = nullptr;
    std::uint32_t* emittedFeeders = nullptr;
    std::uint8_t* emitted = nullptr;
    const float* sendLevelsDb = nullptr;
};

struct ProjectGenerationInstallViewV1 {
    RuntimeProjectGenerationTokenV1 token{};
    std::uint32_t publicationVintage = 1;
    std::uint32_t zero = 0;
    std::uint64_t projectContentGeneration = 0;
    std::uint64_t publishedBusFingerprint = 0;
    std::uint64_t compileTargetProfileFingerprint = 0;
    std::uint64_t ownerId = 0;
    std::uint64_t ownerBaseRevision = 0;
    std::uint64_t ownerRevision = 0;
    ContainerWorldInstallViewV1 compiledWorld{};
    const ProspectiveTopologyView* prospective = nullptr;
    const VnBusId* authoredBusIds = nullptr;
    const float* authoredBusVolumeDb = nullptr;
    const float* authoredBusMakeUpGainDb = nullptr;
    const float* authoredBusOutputVolumeDb = nullptr;
    std::uint32_t authoredBusCount = 0;
};

enum class ProjectGenerationPrepareStatusV1 : std::uint8_t {
    Ok = 0,
    InvalidView = 1,
    Backpressure = 2,
    Capacity = 3,
    CapabilityUnavailable = 4,
    DeviceError = 5,
    InvalidWorld = 6,
};

enum class ProjectGenerationCommitStatusV1 : std::uint8_t {
    Ok = 0,
    Backpressure = 1,
    InvalidPrepared = 2,
    DeviceError = 3,
};

struct ProjectGenerationPrepareResultV1 {
    ProjectGenerationPrepareStatusV1 status =
        ProjectGenerationPrepareStatusV1::InvalidView;
    std::uint64_t preparedSerial = 0;
};

struct ProjectGenerationCommitResultV1 {
    ProjectGenerationCommitStatusV1 status =
        ProjectGenerationCommitStatusV1::InvalidPrepared;
    WorldGeneration installedWorldGeneration = 0;
};

struct SlotAssignmentV1 {
    VnBusId id{};
    std::uint32_t slot = 0xFFFFFFFFu;
    std::uint32_t generation = 0;
    std::uint8_t retain = 0;
    std::uint8_t create = 0;
    std::uint8_t reserved0 = 0;
    std::uint8_t reserved1 = 0;
};
struct SlotPlanV1 {
    bool valid = false;
    bool preservesForeign = false;
    std::uint64_t sourceGraphRevision = 0;
    std::uint32_t sourceBusCount = 0;
    std::vector<std::uint64_t> busOwnerIds;
    std::vector<VnPackHandle> busCookies;
    std::vector<std::uint64_t> sendOwnerIds;
    std::vector<VnPackHandle> sendCookies;
    std::vector<float> sendLevelDb;
    std::vector<std::uint8_t> neutralSlots;
    std::vector<std::uint32_t> migrationTargets;
    std::uint32_t busCount = 0;
    std::uint32_t sendSlotsPerBus = 0;
    std::uint64_t ownerId = 0;
    std::vector<SlotAssignmentV1> assignments;
    std::vector<SlotAssignmentV1> removed;
    std::vector<std::uint8_t> reserved;
    std::vector<std::uint8_t> alive;
    std::vector<VnBusId> busIds;
    std::vector<std::uint32_t> generations;
    std::vector<std::uint32_t> parents;
    std::vector<std::uint32_t> sendTargets;
    std::vector<float> sendLevels;
    std::vector<std::uint8_t> sendLevelApply;
    std::vector<std::uint32_t> sendLevelRampMs;
};

bool BuildSlotPlanFromProspective(Engine& engine, const ProspectiveTopologyView* prospective,
    std::uint64_t ownerId, SlotPlanV1& out);
bool BuildPreviewSlotPlan(Engine& engine, const ProspectiveTopologyView* incoming,
    std::uint64_t ownerId, SlotPlanV1& out);
void ApplySlotPlanToProspective(SlotPlanV1& plan, ProspectiveTopologyView& view) noexcept;
bool SlotPlanMatchesLive(const Engine& engine, const SlotPlanV1& plan) noexcept;
void InstallSlotPlanLogic(Engine& engine, const SlotPlanV1& plan) noexcept;
std::uint32_t FindAssignedSlot(const SlotPlanV1& plan, VnBusId id) noexcept;

class PreparedProjectGenerationV1 {
public:
    PreparedProjectGenerationV1() = default;
    PreparedProjectGenerationV1(const PreparedProjectGenerationV1&) = delete;
    PreparedProjectGenerationV1& operator=(
        const PreparedProjectGenerationV1&) = delete;
    PreparedProjectGenerationV1(PreparedProjectGenerationV1&& other) noexcept {
        MoveFrom(other);
    }
    PreparedProjectGenerationV1& operator=(
        PreparedProjectGenerationV1&& other) noexcept {
        if (this != &other) {
            Reset();
            MoveFrom(other);
        }
        return *this;
    }
    ~PreparedProjectGenerationV1() { Reset(); }

    bool Valid() const noexcept;
    std::uint64_t PreparedSerial() const noexcept { return preparedSerial_; }
    std::uint32_t PreparedPlanIndex() const noexcept {
        return preparedPlanIndex_;
    }
    const RuntimeProjectGenerationTokenV1& Token() const noexcept {
        return token_;
    }

private:
    friend ProjectGenerationPrepareResultV1 PrepareProjectGenerationV1(
        Engine& engine, const ProjectGenerationInstallViewV1& view,
        PreparedProjectGenerationV1& out) noexcept;
    friend ProjectGenerationCommitResultV1 CommitPreparedProjectGenerationV1(
        Engine& engine, PreparedProjectGenerationV1& prepared) noexcept;
    friend void CancelPreparedProjectGenerationV1(
        Engine& engine, PreparedProjectGenerationV1&& prepared) noexcept;

    friend void CancelPreparedProjectGenerationV1(
        PreparedProjectGenerationV1&& prepared) noexcept;

    void Reset() noexcept;
    void ClearFields() noexcept {
        domain_.reset();
        domainEpoch_ = 0;
        engine_ = nullptr;
        occupied_ = false;
        idempotent_ = false;
        preparedPlanIndex_ = UINT32_MAX;
        preparedOwnerSlot_ = UINT32_MAX;
        preparedSerial_ = 0;
        liveOwnerSlot_ = UINT32_MAX;
        expectedOwner_ = {};
        nextOwner_ = {};
        expectedHolder_ = {};
        nextHolder_ = {};
        token_ = {};
        key_ = {};
        publishedBusFingerprint_ = 0;
        compileTargetProfileFingerprint_ = 0;
        publicationVintage_ = 0;
        compiledWorld_ = {};
        worldImage_.Clear();
        livePlanIndex_ = UINT32_MAX;
        slotPlan_ = SlotPlanV1{};
        authoredMixDb_.clear();
        authoredOutputDb_.clear();
        effectiveMixDb_.clear();
        effectiveOutputDb_.clear();
        sourceMixDb_.clear();
        sourceOutputDb_.clear();
        projectedVolumeApply_.clear();
        bindingSourceStamp_ = 0;
    }

    void MoveFrom(PreparedProjectGenerationV1& other) noexcept {
        domain_ = std::move(other.domain_);
        domainEpoch_ = other.domainEpoch_;
        engine_ = other.engine_;
        occupied_ = other.occupied_;
        idempotent_ = other.idempotent_;
        preparedPlanIndex_ = other.preparedPlanIndex_;
        preparedOwnerSlot_ = other.preparedOwnerSlot_;
        preparedSerial_ = other.preparedSerial_;
        liveOwnerSlot_ = other.liveOwnerSlot_;
        expectedOwner_ = other.expectedOwner_;
        nextOwner_ = other.nextOwner_;
        expectedHolder_ = other.expectedHolder_;
        nextHolder_ = other.nextHolder_;
        token_ = other.token_;
        key_ = other.key_;
        publishedBusFingerprint_ = other.publishedBusFingerprint_;
        compileTargetProfileFingerprint_ = other.compileTargetProfileFingerprint_;
        publicationVintage_ = other.publicationVintage_;
        compiledWorld_ = other.compiledWorld_;
        worldImage_ = std::move(other.worldImage_);
        livePlanIndex_ = other.livePlanIndex_;
        slotPlan_ = std::move(other.slotPlan_);
        authoredMixDb_ = std::move(other.authoredMixDb_);
        authoredOutputDb_ = std::move(other.authoredOutputDb_);
        effectiveMixDb_ = std::move(other.effectiveMixDb_);
        effectiveOutputDb_ = std::move(other.effectiveOutputDb_);
        sourceMixDb_ = std::move(other.sourceMixDb_);
        sourceOutputDb_ = std::move(other.sourceOutputDb_);
        projectedVolumeApply_ = std::move(other.projectedVolumeApply_);
        bindingSourceStamp_ = other.bindingSourceStamp_;
        other.ClearFields();
    }

    std::weak_ptr<ProjectGenerationReservationDomainV1> domain_;
    std::uint64_t domainEpoch_ = 0;
    Engine* engine_ = nullptr;
    bool occupied_ = false;
    bool idempotent_ = false;
    std::uint32_t preparedPlanIndex_ = UINT32_MAX;
    std::uint32_t preparedOwnerSlot_ = UINT32_MAX;
    std::uint64_t preparedSerial_ = 0;
    std::uint32_t liveOwnerSlot_ = UINT32_MAX;
    BusTopologyOwnerRecord expectedOwner_{};
    BusTopologyOwnerRecord nextOwner_{};
    PreviewMasterHolderV1 expectedHolder_{};
    PreviewMasterHolderV1 nextHolder_{};
    RuntimeProjectGenerationTokenV1 token_{};
    BusAdmissionClaimKeyV1 key_{};
    std::uint64_t publishedBusFingerprint_ = 0;
    std::uint64_t compileTargetProfileFingerprint_ = 0;
    std::uint32_t publicationVintage_ = 0;
    ContainerWorldInstallViewV1 compiledWorld_{};
    VariableServiceV1::PreparedCurrentWorldImageV1 worldImage_{};
    std::uint32_t livePlanIndex_ = UINT32_MAX;
    SlotPlanV1 slotPlan_{};
    std::vector<float> authoredMixDb_;
    std::vector<float> authoredOutputDb_;
    std::vector<float> effectiveMixDb_;
    std::vector<float> effectiveOutputDb_;
    std::vector<float> sourceMixDb_;
    std::vector<float> sourceOutputDb_;
    std::vector<std::uint8_t> projectedVolumeApply_;
    std::uint64_t bindingSourceStamp_ = 0;
};

ProjectGenerationPrepareResultV1 PrepareProjectGenerationV1(
    Engine& engine, const ProjectGenerationInstallViewV1& view,
    PreparedProjectGenerationV1& out) noexcept;
ProjectGenerationCommitResultV1 CommitPreparedProjectGenerationV1(
    Engine& engine, PreparedProjectGenerationV1& prepared) noexcept;
void CancelPreparedProjectGenerationV1(
    Engine& engine, PreparedProjectGenerationV1&& prepared) noexcept;
void CancelPreparedProjectGenerationV1(
    PreparedProjectGenerationV1&& prepared) noexcept;
std::uint32_t DrainProjectGenerationInstallAcksV1(Engine& engine) noexcept;
bool QueryLiveTopologyOwnerRevisionV1(Engine& engine, std::uint64_t ownerId,
                                      std::uint64_t* outRevision) noexcept;
bool CurrentWorldBusAdmissionIsOpenV1(Engine& engine,
                                      WorldGeneration generation) noexcept;
std::uint32_t LiveMaxBusesV1(const Engine& engine) noexcept;
std::uint32_t LiveBusSendSlotsPerBusV1(const Engine& engine) noexcept;
Engine* ActiveEngineForProjectGenerationV1() noexcept;
void PumpNullMixerForProjectGenerationV1(Engine& engine,
                                         std::uint32_t blockCount) noexcept;

}  // namespace harken

namespace harken::detail {

VnResult PlayVoiceRaw(const VnPlayDesc* desc, VnVoiceHandle* outHandle) noexcept;

}  // namespace harken::detail

#endif
