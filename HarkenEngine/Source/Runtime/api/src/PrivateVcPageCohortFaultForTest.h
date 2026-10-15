#pragma once

namespace harken::detail {

enum class PendingVariablePrepareStageForTest {
    None, NestedControlAcquire, ContinuousDecisionReserve, NestedControlOwnerRegister, VoiceInstall, NestedGroupReserve,
    PreparedGroups, BoundSides, InstanceProofs, ParentProofs, VoiceProofs, TopologyCaptured,
    ContainerProofs, CommandBatchPrepared, SourcePairsPrepared, ReplacementPrepared,
    QueuePreludeBound, QueueSourceControlBound
};
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace harken { struct Engine; struct EnginePreparedTimelineVoiceStart; struct EngineActionPlayScratch; }
namespace harken::detail {
void SetPendingVariablePrepareRefusalHookForTest(
    bool (*hook)(Engine&, EngineActionPlayScratch&, PendingVariablePrepareStageForTest, unsigned, void*),
    void* context) noexcept;
bool RefusePendingVariablePrepareForTest(
    Engine& engine, EngineActionPlayScratch& scratch, PendingVariablePrepareStageForTest stage, unsigned ordinal);
enum class FixedSelectionValidationStageForTest { OriginalBatchesPrepared, CommandHeadReservation };
void SetFixedSelectionValidationHookForTest(
    void (*hook)(Engine&, FixedSelectionValidationStageForTest, void*), void* context) noexcept;
void ObserveFixedSelectionValidationForTest(Engine& engine, FixedSelectionValidationStageForTest stage);
void ForcePrivateVcSecondSideConsumeFailureOnceForTest() noexcept;
bool ConsumePrivateVcSecondSideFailureForTest() noexcept;
void ForcePrivateVcEntryReadUseFailureOnceForTest() noexcept;
bool ConsumePrivateVcEntryReadUseFailureForTest() noexcept;
void ForcePrivateVcArmAdmissionFailureOnceForTest(bool afterPin) noexcept;
bool ConsumePrivateVcArmAdmissionFailureForTest(bool afterPin) noexcept;
void SetTimelineVoiceOwnerCommitRefusalHookForTest(
    bool (*hook)(Engine&, EnginePreparedTimelineVoiceStart&, void*), void* context) noexcept;
bool RefuseTimelineVoiceOwnerCommitForTest(Engine& engine, EnginePreparedTimelineVoiceStart& start);
void SetTimelineVoiceOwnerRollbackRefusalHookForTest(
    void (*hook)(Engine&, EnginePreparedTimelineVoiceStart&, void*), void* context) noexcept;
void ObserveTimelineVoiceOwnerRollbackRefusalForTest(
    Engine& engine, EnginePreparedTimelineVoiceStart& start);

} // namespace harken::detail
#endif
