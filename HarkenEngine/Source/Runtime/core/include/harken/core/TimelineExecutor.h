#ifndef HARKEN_CORE_TIMELINE_EXECUTOR_H
#define HARKEN_CORE_TIMELINE_EXECUTOR_H

#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/TimelineProgram.h"
#include "harken/core/TimelineExecutionOrder.h"
#include "harken/core/AuthoredStopFence.h"

#include <cstdint>
#include <vector>

namespace harken {

inline constexpr std::uint8_t kTimelineDispositionApplied = 0;
inline constexpr std::uint8_t kTimelineDispositionStale = 1;
inline constexpr std::uint8_t kTimelineDispositionArmPending = 2;
inline constexpr std::uint8_t kTimelineDispositionArmFailed = 3;
inline constexpr std::uint8_t kTimelineDispositionStopDominated = 4;
inline constexpr std::uint8_t kTimelineDispositionProtocolViolation = 5;

struct TimelineDigestRecord {
    ConsumedEvent event{};
    TimelineEventPayload payload{};
    TimelineSemanticTarget resolvedBinding{};
    std::uint64_t effectiveFrame = 0;
    std::uint8_t disposition = kTimelineDispositionApplied;
    bool payloadValid = false;
    bool bindingValid = false;
};

class TimelineDigestSink {
public:
    virtual ~TimelineDigestSink() = default;
    virtual void Fold(const TimelineDigestRecord& record) noexcept = 0;
};

struct TimelineSinkEvent {
    std::uint32_t effectiveOffset = 0;
    const TimelineEventPayload* payload = nullptr;
    bool authoredFadeOutCandidate = false;
    TimelineExecutionOrder order{};
};

enum class TimelinePagePrefixStopReason : std::uint8_t {
    ReachedCurrentQuantum = 0,
    PageLimit = 1,
    Unpublished = 2,
    ScratchCapacity = 3,
    Unavailable = 4,
};

struct TimelineExecutionPlan {
    std::uint64_t expectedFrontier = 0;
    std::uint32_t maxPages = 0;
    TimelinePagePrefixStopReason stopReason =
        TimelinePagePrefixStopReason::ReachedCurrentQuantum;
};

struct TimelineExecutionResult {
    std::uint64_t frontier = 0;
    std::uint32_t consumedPageCount = 0;
    TimelinePagePrefixStopReason planStopReason =
        TimelinePagePrefixStopReason::ReachedCurrentQuantum;
};

class TimelineExecutorSink {
public:
    virtual ~TimelineExecutorSink() = default;
    virtual bool BeginQuantum(std::uint64_t quantumIndex) noexcept = 0;
    virtual bool ApplyPlan03Groups(std::uint64_t quantumIndex) noexcept {
        (void)quantumIndex;
        return true;
    }
    virtual bool CollectFrontierGroups(std::uint64_t pageQuantum) noexcept {
        (void)pageQuantum;
        return true;
    }
    virtual bool AdmitQuantum(std::uint64_t quantumIndex) const noexcept {
        (void)quantumIndex;
        return true;
    }
    virtual bool CaptureIndependentWork(std::uint64_t quantumIndex) noexcept {
        (void)quantumIndex;
        return false;
    }
    virtual void DiscardIndependentWork() noexcept {}
    virtual bool BindingValid(std::uint32_t renderSlot,
                              std::uint32_t renderGeneration) const noexcept = 0;
    virtual bool PendingFrontierBindingValid(const ConsumedEvent& event,
        const TimelineEventPayload& payload) const noexcept {
        (void)event;
        (void)payload;
        return false;
    }
    virtual bool ApplyTransaction(std::uint32_t renderSlot,
                                  std::uint32_t renderGeneration,
                                  std::uint64_t quantumIndex,
                                  const TimelineSinkEvent* events,
                                  std::uint32_t eventCount,
                                  TimelineArmToken armToken) noexcept = 0;
    virtual bool CancelBinding(std::uint32_t renderSlot,
                               std::uint32_t renderGeneration) noexcept = 0;
    virtual bool StageFrontierEvent(const ConsumedEvent& event,
        const TimelineEventPayload* payload, std::uint8_t disposition) noexcept {
        (void)event;
        (void)payload;
        (void)disposition;
        return false;
    }
    virtual std::uint8_t FrontierEventDisposition(const ConsumedEvent& event) const noexcept {
        (void)event;
        return kTimelineDispositionProtocolViolation;
    }
    virtual std::uint8_t PlainEventDisposition(const ConsumedEvent& event,
        std::uint8_t disposition) const noexcept {
        (void)event;
        return disposition;
    }
    virtual bool ApplyAuthoredStopFence(
        const AuthoredStopFenceDescriptor& descriptor,
        std::uint64_t quantumIndex) noexcept {
        (void)descriptor;
        (void)quantumIndex;
        return false;
    }
    virtual bool CollectAuthoredStopFence(const AuthoredStopFenceDescriptor& descriptor,
        std::uint64_t quantumIndex, const TimelineExecutionOrder& order) noexcept {
        (void)order;
        return ApplyAuthoredStopFence(descriptor, quantumIndex);
    }
    virtual void FailPlan03Deadline(DecisionToken decision,
                                    std::uint64_t quantumIndex) noexcept {
        (void)decision;
        (void)quantumIndex;
    }
    virtual void BeforeEndQuantum(std::uint64_t quantumIndex,
                                  const TimelineExecutionResult& result) noexcept {
        (void)quantumIndex;
        (void)result;
    }
    virtual void EndQuantum(std::uint64_t quantumIndex) noexcept = 0;
};

class TimelineExecutor {
public:
    struct Counters {
        std::uint64_t deadlineMissQuanta = 0;
        std::uint64_t lateConsumedEvents = 0;
        std::uint64_t renderUnavailableDeferrals = 0;
        std::uint64_t staleSkippedTransactions = 0;
        std::uint64_t armPendingSkipped = 0;
        std::uint64_t armFailedTransactions = 0;
        std::uint64_t spanOverflowDropped = 0;
        std::uint64_t protocolViolation = 0;
        std::uint64_t digest = 14695981039346656037ull;
    };

    bool Reset(std::uint32_t quantumFrames, std::uint32_t maxEventsPerQuantum,
               ScheduledTransactionPool* pool, TimelineArmTable* armTable,
               TimelineExecutorSink* sink,
               DecisionTable* decisionTable = nullptr);
    void SetDigestSink(TimelineDigestSink* digestSink) noexcept {
        digestSink_ = digestSink;
    }
    void ExecuteQuantum(std::uint64_t absoluteQuantum) noexcept;
    TimelineExecutionResult ExecuteQuantum(
        std::uint64_t absoluteQuantum,
        const TimelineExecutionPlan& plan) noexcept;
    void NoteRenderUnavailable() noexcept {
        ++counters_.renderUnavailableDeferrals;
    }
    const Counters& Stats() const { return counters_; }
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    std::size_t DecodedEventCapacityForTest() const noexcept {
        return decoded_.Capacity();
    }
    std::size_t EventStorageBytesForTest() const noexcept {
        return decoded_.RequestedBytes() + nextEvent_.RequestedBytes() + deferredDigest_.RequestedBytes();
    }
    void SetDecodedCommitPageLimitForTest(std::size_t pages) noexcept {
        decoded_.SetCommitPageLimitForTests(pages);
    }
    std::size_t PageGroupCapacityForTest() const noexcept {
        return pageGroups_.size();
    }
#endif

private:
    struct TransactionScratch {
        std::uint32_t firstEvent = UINT32_MAX;
        std::uint32_t lastEvent = UINT32_MAX;
        std::uint32_t eventCount = 0;
        std::uint32_t touchedGeneration = 0;
    };

    void FoldEvent(const ConsumedEvent& event,
                   const TimelineEventPayload* payload,
                   std::uint64_t effectiveFrame,
                   std::uint8_t disposition,
                   const TimelineSemanticTarget* resolvedTarget = nullptr)
        noexcept;
    void CleanupCancelledArms(std::uint64_t audioSequence) noexcept;
    void PrepassPlan03Deadlines(std::uint64_t absoluteQuantum,
                                const TimelineExecutionPlan& plan) noexcept;
    bool PrepareEventStorage(ScheduledTransactionPool::PendingEventStorageDemand demand) noexcept;

    std::uint32_t quantumFrames_ = 0;
    ScheduledTransactionPool* pool_ = nullptr;
    TimelineArmTable* armTable_ = nullptr;
    TimelineExecutorSink* sink_ = nullptr;
    DecisionTable* decisionTable_ = nullptr;
    TimelineDigestSink* digestSink_ = nullptr;
    StableRowStorage<ConsumedEvent, std::size_t> decoded_;
    StableRowStorage<std::uint32_t, std::size_t> nextEvent_;
    std::vector<TransactionScratch> transactions_;
    std::vector<std::uint32_t> touchedTransactions_;
    std::vector<TimelineSinkEvent> sinkEvents_;
    std::vector<Plan03PageGroupView> pageGroups_;
    StableRowStorage<TimelineDigestRecord, std::size_t> deferredDigest_;
    ScheduledTransactionPool::PendingEventStorageRegistration eventStorageRegistration_;
    std::size_t deferredDigestCount_ = 0;
    bool deferDigest_ = false;
    Counters counters_{};
    std::uint32_t touchGeneration_ = 1;
    bool valid_ = false;
};

} // namespace harken

#endif // HARKEN_CORE_TIMELINE_EXECUTOR_H
