#ifndef HARKEN_API_PROJECT_GENERATION_TRANSACTION_H
#define HARKEN_API_PROJECT_GENERATION_TRANSACTION_H

#include "TopologySlotPlan.h"
#include <harken/ObjectId.h>
#include "PlayableRuntime.h"
#include "SignalPublication.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/EngineConfig.h"
#include "harken/param/VariableService.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace harken {

bool HasPredecessorDirectWorldPublication(const Engine& engine) noexcept;

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
        preservesContent_ = false;
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
        signals_ = {};
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
        preservesContent_ = other.preservesContent_;
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
        signals_ = std::move(other.signals_);
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
    bool preservesContent_ = false;
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
    PreparedSignalPublication signals_{};
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

#endif
