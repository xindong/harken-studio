#ifndef HARKEN_RENDER_FRONTIER_TIMELINE_FINALIZER_H
#define HARKEN_RENDER_FRONTIER_TIMELINE_FINALIZER_H

#include "harken/core/TimelineExecutor.h"

#include <memory>

namespace harken {

class VoicePool;
class VariableControlInstanceTable;
class QueueInstanceTable;
class OccurrenceStatePool;
class CompositeCompletionOwnerPool;
class ArmGroupTable;
class TransitionInstancePool;
class ImmediateActionLane;
class ImmediateLaneBucketClaimReceipt;
struct ImmediateLaneBucket;
struct ImmediateLaneEvent;
class VoiceTimelineReservationProof;
class VariableTimelineInitializerProof;

// Collects immutable due work before the single serial pre-render boundary.
class FrontierTimelineFinalizer {
public:
    FrontierTimelineFinalizer(VoicePool& voices, TimelineArmTable& arms);
    ~FrontierTimelineFinalizer();
    bool Attach(DecisionTable& decisions, const ScheduledTransactionPool& pool,
                bool laneOnly = false);
    void AttachVariableControls(VariableControlInstanceTable& instances) noexcept;
    void AttachQueues(QueueInstanceTable& queues) noexcept;
    void AttachQueueCompletion(const OccurrenceStatePool& occurrences,
        const CompositeCompletionOwnerPool* composites = nullptr) noexcept;
    void AttachArmGroups(const ArmGroupTable& groups) noexcept;
    void AttachTransitions(TransitionInstancePool& transitions) noexcept;
#if !defined(HARKEN_RT_QUALIFICATION)
    void SetVariableControlSliceClaimHookForTest(void (*hook)(void*), void* context) noexcept;
    void SetQueueFutureSliceHooksForTest(void (*validated)(void*), void (*claimed)(void*),
        void (*applied)(void*, std::uint16_t), void* context) noexcept;
    void SetQueueLaneProbeGroupForTest(Plan03GroupSliceIdentity group) noexcept;
#endif
    void Begin(std::uint64_t quantum, std::uint32_t blockFrames) noexcept;
    void BeginLane(std::uint64_t quantum, std::uint32_t blockFrames,
        const ImmediateActionLane& lane, const ImmediateLaneBucket* bucket,
        std::uint32_t slot) noexcept;
    VariableReadCutStatus VariableSourcesStatus(
        FrontierEntryReceiptRef* waitingEntries = nullptr, std::uint32_t capacity = 0,
        std::uint32_t* waitingEntryCount = nullptr) noexcept;
    VariableReadCutStatus LaneEventTimingStatus(const ConsumedEvent& event) const noexcept;
    bool LaneEventOffset(const ConsumedEvent& event, std::uint16_t* outOffset) const noexcept;
    bool CollectGroup(const Plan03PageGroupView& group) noexcept;
    bool PendingBindingValid(const ConsumedEvent& event,
        const TimelineEventPayload& payload) const noexcept;
    bool CollectEvent(const ConsumedEvent& event, const TimelineEventPayload* payload,
                      std::uint8_t validationDisposition) noexcept;
    bool CollectLaneStagedStart(const ImmediateLaneEvent& event,
        const TimelineArmTable::LaneAudioProof& arm,
        const VoiceTimelineReservationProof& reservation,
        const VariableTimelineInitializerProof& initializer) noexcept;
    bool CollectStop(const AuthoredStopFenceDescriptor& descriptor,
                     const TimelineExecutionOrder& order) noexcept;
    void Finish() noexcept;
    // A lane bucket exposes all its entry coordinators or none of them.
    bool FinishLaneEntryBucket(
        bool (*claimBucket)(void*, ImmediateLaneBucketClaimReceipt*) noexcept,
        void* claimContext, const ImmediateActionLane* lane,
        const ImmediateLaneBucket* bucket, std::uint32_t laneSlot,
        std::uint32_t laneGeneration) noexcept;
    bool FinishLaneContinuationBucket(
        bool (*claimBucket)(void*, ImmediateLaneBucketClaimReceipt*) noexcept,
        void* claimContext, const ImmediateActionLane* lane,
        const ImmediateLaneBucket* bucket, std::uint32_t laneSlot,
        std::uint32_t laneGeneration) noexcept;
    std::uint8_t EventDisposition(const ConsumedEvent& event) const noexcept;
    std::uint64_t StorageBytes() const noexcept;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    std::uint32_t EventCapacityForTest() const noexcept;
    std::uint32_t GroupCapacityForTest() const noexcept;
#endif

private:
    bool FinishLaneBucketImpl(
        bool (*claimBucket)(void*, ImmediateLaneBucketClaimReceipt*) noexcept,
        void* claimContext, const ImmediateActionLane* lane,
        const ImmediateLaneBucket* bucket, std::uint32_t laneSlot,
        std::uint32_t laneGeneration, bool continuation) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace harken

#endif // HARKEN_RENDER_FRONTIER_TIMELINE_FINALIZER_H
