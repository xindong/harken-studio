// Private probe binding front end and lifecycle orchestration. The probe
// detector rows and their ballistics live in MeterSystem (the single drain
// owner); this layer owns the probe-scoped binding definitions and
// attachments, the withdrawal walk that folds their contributions back to
// neutral, and the engine-session entry points.
#ifndef HARKEN_PARAM_METER_PROBE_H
#define HARKEN_PARAM_METER_PROBE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/BusSlotRef.h"
#include "harken/param/Curve.h"
#include "harken/param/BindingTable.h"
#include "harken/param/MeterSystem.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace harken {

struct Engine;
struct ProjectGenerationReservationDomainV1;

// Trace/capture view of one probe binding definition row (the shared
// BindingTable row rendered in the probe vocabulary).
struct MeterProbeBindingDefinition {
    VnObjectId bindingId{};
    VnObjectId probeStableId{};
    std::uint32_t probeGeneration = 0;
    std::uint32_t targetScope = 0;
    std::uint32_t targetId = 0;
    std::uint32_t transitionMs = 0;
    Curve curve{};
};

// Trace/capture view of one probe attachment row.
struct MeterProbeAttachment {
    VnObjectId bindingId{};
    std::uint32_t targetScope = 0;
    VnObjectId targetStableId{};
    VnVoiceHandle targetInstanceHandle = 0;
    std::uint32_t targetGeneration = 0;
};

// Logic-thread orchestration over detector rows, shared binding rows, and
// bounded withdrawal scratch. Audible withdrawal targets remain owned by the
// shared VariableSliceTable.
class MeterProbeSystem {
public:
    MeterProbeSystem(std::uint32_t maxBindingAttachments = 1,
                     std::uint32_t maxTargetsPerCurrentSlice = 1);

    VnResult Install(Engine& engine, VnObjectId probeStableId,
                     VnBusId busId, std::uint32_t tap,
                     std::uint32_t attackMs, std::uint32_t releaseMs,
                     std::uint32_t* installedGeneration = nullptr);
    VnResult Uninstall(Engine& engine, VnObjectId probeStableId,
                       std::uint32_t expectedGeneration);
    VnResult DefineBinding(Engine& engine, VnObjectId bindingId,
                           VnObjectId probeStableId,
                           std::uint32_t probeGeneration,
                           std::uint32_t targetScope,
                           std::uint32_t targetId,
                           const CurvePoint* points,
                           const CurveHandlePair* handles,
                           std::uint32_t pointCount,
                           std::uint32_t transitionMs);
    VnResult UndefineBinding(Engine& engine, VnObjectId bindingId);
    VnResult Attach(Engine& engine, VnObjectId bindingId,
                    std::uint32_t targetScope,
                    VnObjectId targetStableId, VnVoiceHandle targetInstanceHandle,
                    std::uint32_t targetGeneration);
    VnResult Detach(Engine& engine, VnObjectId bindingId,
                    std::uint32_t targetScope,
                    VnObjectId targetStableId, VnVoiceHandle targetInstanceHandle,
                    std::uint32_t targetGeneration);

    class PreparedBusRemovalV1 {
    public:
        PreparedBusRemovalV1() = default;
        PreparedBusRemovalV1(const PreparedBusRemovalV1&) = delete;
        PreparedBusRemovalV1& operator=(const PreparedBusRemovalV1&) = delete;

    private:
        friend class MeterProbeSystem;
        struct ProbeRow {
            std::uint32_t slot = 0;
            MeterProbeIdentity identity{};
            MeterSystem::State state = MeterSystem::State::Free;
            std::uint32_t sourceBusIndex = 0;
            std::uint64_t withdrawalSequence = 0;
            std::uint64_t issuedSequence = 0;
        };
        struct AttachmentRow {
            BindingTable::Attachment* address = nullptr;
            BindingTable::Attachment expected{};
            VnObjectId definitionId{};
            VnObjectId probeStableId{};
            std::uint32_t probeGeneration = 0;
            std::uint32_t definitionRevision = 0;
            std::uint32_t transitionMs = 0;
            std::uint64_t definitionOwnerCookie = 0;
            bool release = false;
        };
        Engine* owner = nullptr;
        std::weak_ptr<ProjectGenerationReservationDomainV1> domain;
        std::uint64_t domainEpoch = 0;
        std::vector<BusSlotRef> removed;
        std::vector<ProbeRow> probes;
        std::vector<AttachmentRow> attachments;
    };

    VnResult PrepareBusRemovalV1(Engine& engine, const std::vector<BusSlotRef>& removed,
                                 PreparedBusRemovalV1& out);
    bool CanCommitBusRemovalV1(const Engine& engine, const PreparedBusRemovalV1& prepared) const noexcept;
    void CommitBusRemovalV1(Engine& engine, PreparedBusRemovalV1& prepared) noexcept;

    void Evaluate(Engine& engine);
    void EndService(Engine& engine);
    void NoteBusDestroyed(Engine& engine, std::uint32_t busIndex,
                          std::uint32_t busGeneration);
    void NoteVoiceTerminal(Engine& engine, VnVoiceHandle publicHandle,
                           VnVoiceHandle renderHandle);

    std::uint32_t BindingCount(const Engine& engine) const noexcept;
    std::uint32_t AttachmentCount(const Engine& engine) const noexcept;
    VariableSliceRef WithdrawalSliceForTests(
        const Engine& engine, VnObjectId probeStableId,
        std::uint32_t probeGeneration) const noexcept;

private:
    struct WithdrawalTarget {
        VariableTargetLocator locator{};
        std::uint32_t revision = 0;
        std::uint32_t rampMs = 0;
    };

    void RetireProbe(Engine& engine, MeterSystem::Probe& probe,
                     std::uint64_t logicSequence) noexcept;
    bool ResolveWithdrawalTarget(
        const Engine& engine, const BindingTable::Attachment& attachment,
        const BindingTable::Definition& definition,
        WithdrawalTarget* out) const noexcept;
    bool PrepareWithdrawalChunk(Engine& engine,
                                MeterSystem::Probe& probe);
    bool AttachmentInWithdrawalChunk(
        const Engine& engine, const MeterSystem::Probe& probe,
        const BindingTable::Attachment& attachment,
        const BindingTable::Definition& definition) const noexcept;
    bool CaptureAndScheduleWithdrawalChunk(Engine& engine,
                                           MeterSystem::Probe& probe);
    void ReleaseWithdrawalChunkRows(Engine& engine,
                                    MeterSystem::Probe& probe);
    void ReleaseSliceClaims(Engine& engine,
                            VariableSliceRef slice) noexcept;
    void CommitSliceTargets(Engine& engine,
                            VariableSliceRef slice) noexcept;

    std::vector<WithdrawalTarget> withdrawalScratch_;
    std::uint32_t maxTargetsPerCurrentSlice_ = 1;
};

VnResult InstallMeterProbe(Engine& engine, VnObjectId probeStableId,
                           VnBusId busId, std::uint32_t tap,
                           std::uint32_t attackMs, std::uint32_t releaseMs,
                           std::uint32_t* installedGeneration = nullptr);
VnResult UninstallMeterProbe(Engine& engine, VnObjectId probeStableId,
                             std::uint32_t expectedGeneration);
VnResult DefineMeterProbeBinding(
    Engine& engine, VnObjectId bindingId, VnObjectId probeStableId,
    std::uint32_t probeGeneration, std::uint32_t targetScope,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs);
VnResult UndefineMeterProbeBinding(Engine& engine, VnObjectId bindingId);
VnResult AttachMeterProbeBinding(Engine& engine, VnObjectId bindingId,
                                 std::uint32_t targetScope,
                                 VnObjectId targetStableId, VnVoiceHandle targetInstanceHandle,
                                 std::uint32_t targetGeneration);
VnResult DetachMeterProbeBinding(Engine& engine, VnObjectId bindingId,
                                 std::uint32_t targetScope,
                                 VnObjectId targetStableId, VnVoiceHandle targetInstanceHandle,
                                 std::uint32_t targetGeneration);

} // namespace harken

#endif // HARKEN_PARAM_METER_PROBE_H
