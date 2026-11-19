#ifndef HARKEN_API_TOPOLOGY_SLOT_PLAN_H
#define HARKEN_API_TOPOLOGY_SLOT_PLAN_H

#include <harken/ObjectId.h>
#include "PlayableRuntime.h"
#include "SignalPublication.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/EngineConfig.h"
#include "harken/media/PackFormatV2.h"
#include "harken/param/VariableService.h"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace harken {

struct Engine;
struct ProjectGenerationReservationDomainV1;
struct BusSlotRef;

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
bool PreviewMasterCompatible(const Engine& engine, std::uint64_t fingerprint,
    std::uint64_t ownerId = 0) noexcept;
bool HasPreviewEffectAttachment(const Engine& engine, BusSlotRef host) noexcept;
std::uint32_t SelectPreviewOwnerDestination(const Engine& engine, std::uint64_t ownerId) noexcept;
inline constexpr std::uint64_t kCanonicalPackSetMasterOwnerId = 0x5053434E00000001ull;
bool CanResetNeutralBusPropertiesV1(const Engine& engine, std::uint32_t slot,
    std::uint64_t allowedEffectOwner = 0) noexcept;
void ResetNeutralBusPropertiesV1(Engine& engine, std::uint32_t slot) noexcept;
bool CanResetVolumeOnlyNonVolumePropertiesV2(
    const Engine& engine, std::uint32_t slot,
    std::uint64_t allowedOwnerCookie = 0,
    std::uint64_t allowedEffectOwner = 0) noexcept;
void ResetVolumeOnlyNonVolumePropertiesV2(Engine& engine,
    std::uint32_t slot, bool preserveEffects = false) noexcept;
bool BuildBusEffectParameterCommandV1(Engine& engine, std::uint32_t busIndex,
    std::uint32_t slot, std::uint32_t paramIndex, std::span<const float> parameters,
    std::uint32_t type, CommandRecord* out) noexcept;
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

class PreparedBusEffectsV1 {
public:
    PreparedBusEffectsV1();
    ~PreparedBusEffectsV1();
    PreparedBusEffectsV1(const PreparedBusEffectsV1&) = delete;
    PreparedBusEffectsV1& operator=(const PreparedBusEffectsV1&) = delete;
    VnResult Prepare(Engine& engine, const SlotPlanV1& plan,
        std::span<const PackBusDefinitionEffect> effects,
        std::span<const PackBusDefinitionEffectParameter> parameters,
        std::uint64_t allowedOwner, std::uint64_t nextOwner,
        bool deferStructureWhilePlaying = false);
    bool CanCommit() const noexcept;
    void Commit() noexcept;
    std::span<const CommandRecord> Commands() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};


#if HARKEN_ENGINE_DEV
std::uint32_t DrainProjectGenerationInstallAcksV1(Engine& engine) noexcept;
#else
inline std::uint32_t DrainProjectGenerationInstallAcksV1(Engine&) noexcept { return 0; }
#endif

}  // namespace harken

namespace harken::detail {

VnResult PlayVoiceRaw(const VnPlayDesc* desc, VnVoiceHandle* outHandle) noexcept;

}  // namespace harken::detail

#endif
