// Fixed-capacity voice pool with generational handles. Voice rows are prepared
// on the logic thread before publication and stay stable through retirement.
// Audio-side starts and endings never allocate. Over-capacity Play refuses
// with VN_NO_VOICE; a stale handle is a benign no-op with a counter.
//
// Voice lifecycle across the command transport: the logic side claims a
// slot, mints the generation, and fully prepares the render context; the
// slot starts rendering only when the audio side applies the queued play
// command at a block boundary. Slot release (natural end, fade completion,
// or an applied stop) happens on the audio side and emits a completion
// notification.
//
// Capacity terminology: every slot in this pool is a Render Voice. Public
// Logical Voice identity and parked Virtual Voice state live outside it.
#ifndef HARKEN_RENDER_VOICE_POOL_H
#define HARKEN_RENDER_VOICE_POOL_H

#include <harken/Harken.h>

#include "harken/core/BlockArena.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/NotificationQueue.h"
#include "harken/core/MonitorVoiceControl.h"
#include "harken/core/PcmView.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/core/SourceCursorMirror.h"
#include "harken/core/SourceTerminalRecord.h"
#include "harken/core/StableRowStorage.h"
#include "harken/core/StableSlotStorage.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/VariableReadCut.h"
#include "harken/core/StreamParkMailbox.h"
#include "harken/core/StreamSeekHandoffTable.h"
#include "harken/dsp/Resampler.h"
#include "harken/render/EffectPool.h"
#include "harken/render/DynamicRoute.h"
#include "harken/render/RenderWorkerPool.h"
#include "harken/render/StreamParkToken.h"
#include "harken/render/Voice.h"
#include "harken/render/OriginalSourceInstallationLoan.h"
#include "harken/render/OriginalSourceMutationActor.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace harken {

class DecisionTable;
class OccurrenceStatePool;
class OriginalSourceSubmissionRegistry;
namespace detail { struct VoicePoolReleaseTestAccess; }

struct VariableTimelineStartAuthority {
    void* context = nullptr;
    VariableReadCutStatus (*ready)(void*, VariableTimelineReadUseRef, std::uint64_t) noexcept = nullptr;
    bool (*provePlain)(void*, VariableTimelineReadUseRef, std::uint64_t,
        const TimelineArmTable::PlainAudioProof&, VariableTimelineInitializerProof*) noexcept = nullptr;
    bool (*proveLane)(void*, VariableTimelineReadUseRef, std::uint64_t,
        const TimelineArmTable::LaneAudioProof&, VariableTimelineInitializerProof*) noexcept = nullptr;
    void (*apply)(VariableTimelineInitializerProof&, void*,
        void (*)(void*, const CommandRecord&) noexcept) noexcept = nullptr;
    bool (*proveBound)(void*, VariableTimelineReadUseRef, std::uint64_t,
        const TimelineArmTable::BoundAudioProof&, VariableTimelineInitializerProof*) noexcept = nullptr;
    bool (*proveOperation)(void*, VariableTimelineReadUseRef, std::uint64_t,
        const TimelineArmTable::AudioOperationClaim&, VariableTimelineInitializerProof*) noexcept = nullptr;
    VariableReadCutStatus (*entryReady)(void*, const FrontierActivationDescriptor&) noexcept = nullptr;
    bool (*pendingArm)(void*, VariableTimelineReadUseRef, std::uint64_t, TimelineArmToken) noexcept = nullptr;
    bool Valid() const noexcept { return ready != nullptr; }
};

struct Plan03ScratchDecisionState {
    std::uint32_t generation = 0;
    bool currentSliceClaimed = false;
};

struct VoiceTimelineReservation {
    std::uint32_t renderSlot = UINT32_MAX;
    std::uint32_t renderGeneration = 0;
    std::uint64_t quantum = 0;
    std::uint64_t epoch = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

class VoiceTimelineReservationProof {
public:
    VoiceTimelineReservationProof() = default;
    bool Valid() const noexcept { return events_ != nullptr && count_ != 0 && claims_ != nullptr; }
    ~VoiceTimelineReservationProof() { Release(); }
    VoiceTimelineReservationProof(const VoiceTimelineReservationProof&) = delete;
    VoiceTimelineReservationProof& operator=(const VoiceTimelineReservationProof&) = delete;
    VoiceTimelineReservationProof(VoiceTimelineReservationProof&& other) noexcept
        : events_(other.events_), count_(other.count_), claims_(other.claims_), mask_(other.mask_) {
        other.events_ = nullptr;
        other.count_ = 0;
        other.claims_ = nullptr;
    }

private:
    friend class VoicePool;
    void Release() noexcept {
        if (claims_ != nullptr) *claims_ &= ~mask_;
        events_ = nullptr;
        count_ = 0;
        claims_ = nullptr;
    }
    VoiceTimelineEvent* events_ = nullptr;
    std::uint32_t count_ = 0;
    std::uint32_t* claims_ = nullptr;
    std::uint32_t mask_ = 0;
};

class VoicePool {
    struct alignas(64) Slot;
public:
    VoicePool(const EngineConfig& config, const ResampleTables* tables,
              const MultirateResampleTables* multirateTables,
              const BiquadTrigTable* biquadTrig, EffectPoolBank& effectBank,
              DynamicRouteSystem& dynamicRoutes,
              TimelineArmTable* timelineArms = nullptr);
    ~VoicePool();
    VoicePool(const VoicePool&) = delete;
    VoicePool& operator=(const VoicePool&) = delete;
    VoicePool(VoicePool&&) = delete;
    VoicePool& operator=(VoicePool&&) = delete;

    // Claims a Render Voice slot and prepares its context for the given PCM.
    // `pitchRatio` is the caller's rate multiplier; the source-vs-engine
    // sample-rate mismatch is folded in here so the resampler sees one
    // combined ratio. `spatialized` selects the bus fold-in law for the
    // voice's whole life; the position seeds the render-side mirror and
    // initialPan seeds the exact layout-resolved fold before its first block. The
    // loop region [loopStartFrame, loopEndFrame) and loopCount (1 = single
    // pass, 0 = infinite, N = N passes) are resolved on the logic side and
    // travel into the render context here. resampleProfile is likewise
    // selected by the logic-side play authority and is never re-derived here.
    // The returned handle is valid immediately; rendering begins when
    // ApplyPlay runs on the audio side. Logic thread only.
    VnResult Play(const PCMView& view, float volumeDb, double pitchRatio,
                  std::uint32_t busIndex, bool spatialized, float posX,
                  float posY, float posZ, const ResolvedPanGains& initialPan,
                  std::uint64_t loopStartFrame, std::uint64_t loopEndFrame,
                  std::uint32_t loopCount, ResampleProfile resampleProfile,
                  VnVoiceHandle* outHandle,
                  std::uint32_t busGeneration = 1,
                  std::uint32_t requestedSlot = ~std::uint32_t{0},
                  std::uint32_t expectedGeneration = 0,
                  bool prepared = false, float outputBusVolumeDb = 0.0f,
                  float spatializationMix = 1.0f, bool prepareOwnedWeights = true);

    // Claims a Render Voice slot for a streamed Logical Voice. The independent
    // stream slot is owned by StreamManager; the source decode stage feeds
    // from `ring` instead of registered PCM, gated on prebufferTargetFrames
    // (clamped to the ring's capacity — a full ring is as prebuffered as it
    // can get). channelCount and sourceSampleRate are the stream's facts,
    // resolved on the logic side; they seed a synthetic PCMView (frames 0,
    // no use count) so the cascade's channel and rate plumbing is shared
    // with registered-PCM voices. Stream looping is producer-owned, so no
    // loop fields travel into the render context. Logic thread only; the
    // caller wires the stream slot Active only after this succeeds.
    VnResult PlayStream(StreamRing* ring, std::uint32_t channelCount,
                        std::uint32_t sourceSampleRate,
                        std::uint32_t prebufferTargetFrames, float volumeDb,
                        double pitchRatio, std::uint32_t busIndex,
                        bool spatialized, float posX, float posY, float posZ,
                        const ResolvedPanGains& initialPan,
                        std::uint64_t activationStartFrame,
                        std::uint64_t frameCount,
                        std::uint64_t loopStartFrame,
                        std::uint64_t loopEndFrame,
                        std::uint32_t loopsRemaining,
                        ResampleProfile resampleProfile,
                        VnVoiceHandle* outHandle,
                        std::uint32_t busGeneration = 1,
                        std::uint32_t requestedSlot = ~std::uint32_t{0},
                        std::uint32_t expectedGeneration = 0,
                        const StreamParkTokenRef* resumeToken = nullptr,
                        float outputBusVolumeDb = 0.0f,
                        bool prepared = false,
                        float spatializationMix = 1.0f);

    bool BindStreamRingToken(VnVoiceHandle handle, StreamRing* ring,
                             std::uint64_t ringToken) noexcept;

    void AttachNativeVariableStartAuthority(VariableNativeStartAuthority authority) noexcept {
        m_nativeVariableStartAuthority = authority;
    }
    void AttachTimelineVariableStartAuthority(VariableTimelineStartAuthority authority) noexcept {
        m_timelineVariableStartAuthority = authority;
    }
    bool BindPreparedTimelineReadUse(VnVoiceHandle handle, VariableTimelineReadUseRef use,
        VariablePlaybackOrder order, bool pendingPlacement = false) noexcept;
    bool ProveTimelineVariableInitializer(std::uint32_t renderSlot, std::uint32_t renderGeneration,
        const TimelineArmTable::PlainAudioProof& arm, VariableTimelineInitializerProof* out) noexcept;
    bool ProveTimelineVariableInitializer(std::uint32_t renderSlot, std::uint32_t renderGeneration,
        const TimelineArmTable::LaneAudioProof& arm, VariableTimelineInitializerProof* out) noexcept;
    bool ProveTimelineVariableInitializer(std::uint32_t renderSlot, std::uint32_t renderGeneration,
        const TimelineArmTable::BoundAudioProof& arm, VariableTimelineInitializerProof* out) noexcept;
    bool ProveTimelineVariableInitializer(std::uint32_t renderSlot, std::uint32_t renderGeneration,
        const TimelineArmTable::AudioOperationClaim& arm, VariableTimelineInitializerProof* out) noexcept;
    void ApplyTimelineVariableInitializer(VariableTimelineInitializerProof& proof) noexcept;
    VariableReadCutStatus TimelineVariableSourcesStatus(std::uint32_t renderSlot,
        std::uint32_t renderGeneration) const noexcept;
    VariableReadCutStatus TimelineStartSourcesStatus(const TimelineArmTable& arms,
        TimelineArmToken arm, bool* waitingPublication = nullptr) const noexcept;
    VariableReadCutStatus TimelineEntryVariableSourcesStatus(
        const FrontierActivationDescriptor& descriptor) const noexcept;
    bool PendingTimelineVariableReadMatchesArm(TimelineArmToken arm) const noexcept;
    bool HasTimelineVariableSourceRead(std::uint32_t renderSlot,
        std::uint32_t renderGeneration) const noexcept;
    VariableTimelineReadUseRef TimelineVariableSourceRead(std::uint32_t renderSlot,
        std::uint32_t renderGeneration) const noexcept;
    // Audio detaches its final source reader only after the original start claim succeeds.
    void FinishTimelineVariableSourceRead(std::uint32_t renderSlot,
        std::uint32_t renderGeneration) noexcept;
    // Logic binds only while the voice is private; the audio side owns it after Play.
    bool BindPreparedNativeReadCut(VnVoiceHandle handle,
                                   const VariableNativeStartIdentity& identity) noexcept;
    bool FinishNativeVariableStartInstallation(VnVoiceHandle handle) noexcept;
    bool HasPendingNativeVariableStart(VnVoiceHandle handle) noexcept;
    void RejectNativeVariableStart(VnVoiceHandle handle, NotificationQueue& notifications,
                                    std::uint64_t blockIndex) noexcept;
    void ActivatePendingNativeVariableStarts(NotificationQueue& notifications,
                                              std::uint64_t blockIndex) noexcept;
    bool BindPreparedFirstOutputOrder(VnVoiceHandle handle, VariablePlaybackOrder order) noexcept;
    bool DeferVariableCommandUntilFirstOutput(const CommandRecord& command, VariablePlaybackOrder order,
        std::uint32_t targetRevision, VariableNativeEffectIdentity effect = {},
        VariableToneBandValues toneBand = {}) noexcept;
    void AttachFirstOutputCommandConsumer(void* context,
        void (*apply)(void*, const CommandRecord&) noexcept) noexcept {
        m_firstOutputCommandContext = context;
        m_firstOutputCommandConsumer = apply;
    }
    std::size_t FirstOutputStorageBytes() const noexcept;
    // Backing only: native timeline authority still governs every pending event.
    std::span<VoiceTimelineEvent> PendingTimelineVariableEvents(std::uint32_t renderSlot) noexcept {
        if (renderSlot >= m_pendingTimelineVariableEvents.ConstructedCount()) return {};
        return m_pendingTimelineVariableEvents[renderSlot];
    }
    void SetPendingTimelineVariableEventCommitPageLimitForTests(std::size_t pages) noexcept {
        m_pendingTimelineVariableEvents.SetCommitPageLimitForTests(pages);
    }
    bool CaptureStreamRingToken(VnVoiceHandle handle,
                                std::uint64_t* outToken) const noexcept;

    // Non-counting free-slot predicate: true when a Free slot exists. Unlike
    // CheckCapacityForPlay this never touches the refusal counter, so the
    // logic side can branch on pool fullness (to choose the steal path)
    // without recording a refusal that may not happen. Logic thread only; the
    // observed slot stays Free until this thread claims it.
    bool HasFreeSlot() const;
    std::uint32_t AvailableSlotCount() const;

    // Returns the current generation when the exact Render slot is Free.
    // The logic thread is the only claimant, so an observed Free slot remains
    // available until that thread claims it. Exact-slot admission lets the
    // Voice Manager honor a victim-generation promise without another start
    // consuming the released slot.
    bool FreeSlotGeneration(std::uint32_t slot,
                            std::uint32_t* outGeneration) const;
    bool CaptureReleaseObserver(VnVoiceHandle handle,
                                VoiceReleaseObserver* out) const noexcept;
    bool ReleaseObserved(const VoiceReleaseObserver& observer) const noexcept;
    bool BindMonitorControl(VnVoiceHandle handle, const MonitorVoiceControlBinding& binding) noexcept;
    bool BindSourceTerminal(VnVoiceHandle handle, SourceTerminalRecord* record,
                            SourceTerminalToken token) noexcept;
    std::uint32_t Capacity() const {
        return m_slots.Capacity();
    }
    std::uint32_t ConstructedSlotCount() const noexcept { return m_slots.ConstructedCount(); }
    // Control-side bound for source plus max(blend, send) planes in every row.
    std::uint64_t PreparedCascadeChannelRows(
        std::uint32_t mixChannels, std::uint32_t slots) const noexcept {
        std::uint64_t rows = static_cast<std::uint64_t>(mixChannels) * slots;
        for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
            const auto count = m_ringStorage[ch].ConstructedCount();
            rows += count;
            if (ch >= mixChannels) rows += count;
        }
        return rows;
    }
    std::size_t SlotStorageBytes() const noexcept { return m_slots.RequestedBytes(); }
    // Attach and detach only while the corresponding consumer is quiescent.
    class SlotStorageRegistration {
    public:
        using Prepare = bool (*)(void*, std::uint32_t) noexcept;
        SlotStorageRegistration() = default;
        ~SlotStorageRegistration() { Detach(); }
        SlotStorageRegistration(const SlotStorageRegistration&) = delete;
        SlotStorageRegistration& operator=(const SlotStorageRegistration&) = delete;
        void Detach() noexcept;
    private:
        friend class VoicePool;
        VoicePool* owner_ = nullptr;
        SlotStorageRegistration* previous_ = nullptr;
        SlotStorageRegistration* next_ = nullptr;
        void* context_ = nullptr;
        Prepare prepare_ = nullptr;
    };
    bool AttachSlotStoragePreparation(SlotStorageRegistration& registration,
        void* context, SlotStorageRegistration::Prepare prepare) noexcept;
    bool PrepareSlotBacking(std::uint32_t count, StableStoragePreparation* out) noexcept;
    bool PrepareMonitorPreChainBacking() noexcept;
    bool PrepareSlotWorkingSet(std::uint32_t renderSlot, std::uint32_t channelCount,
        ResampleProfile profile = ResampleProfile::LegacyCompatibility,
        bool prepareOwnedWeights = true, std::uint32_t highRatioChannels = 0) noexcept;
    std::size_t SlotCommittedBytes() const noexcept { return m_slots.CommittedBytes(); }
    std::size_t SlotReservedBytes() const noexcept { return m_slots.ReservedBytes(); }
    std::size_t RenderWorksetStorageBytes() const noexcept {
        std::size_t bytes = m_firstOutputEndpoints.RequestedBytes() + m_firstOutputWeights.RequestedBytes() +
            m_pendingTimelineVariableEvents.RequestedBytes() +
            m_professionalStorage.RequestedBytes() + m_variableWeightStorage.RequestedBytes();
        for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch)
            bytes += m_ringStorage[ch].RequestedBytes() + m_monitorPreChainStorage[ch].RequestedBytes() +
                m_highRatioStorage[ch].RequestedBytes();
        return bytes;
    }
    std::size_t RenderWorksetCommittedBytes() const noexcept {
        std::size_t bytes = m_firstOutputEndpoints.CommittedBytes() + m_firstOutputWeights.CommittedBytes() +
            m_pendingTimelineVariableEvents.CommittedBytes() +
            m_professionalStorage.CommittedBytes() + m_variableWeightStorage.CommittedBytes();
        for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch)
            bytes += m_ringStorage[ch].CommittedBytes() + m_monitorPreChainStorage[ch].CommittedBytes() +
                m_highRatioStorage[ch].CommittedBytes();
        return bytes;
    }
    std::size_t RenderWorksetReservedBytes() const noexcept {
        std::size_t bytes = m_firstOutputEndpoints.ReservedBytes() + m_firstOutputWeights.ReservedBytes() +
            m_pendingTimelineVariableEvents.ReservedBytes() +
            m_professionalStorage.ReservedBytes() + m_variableWeightStorage.ReservedBytes();
        for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch)
            bytes += m_ringStorage[ch].ReservedBytes() + m_monitorPreChainStorage[ch].ReservedBytes() +
                m_highRatioStorage[ch].ReservedBytes();
        return bytes;
    }
    void SetSlotCommitPageLimitForTests(std::size_t pages) noexcept {
        m_slots.SetCommitPageLimitForTests(pages);
    }
    void SetStreamParkCommitPageLimitForTests(std::size_t pages) noexcept {
        m_streamParkTokens.SetCommitPageLimitForTests(pages);
    }
    void SetProfessionalCommitPageLimitForTests(std::size_t pages) noexcept {
        m_professionalStorage.SetCommitPageLimitForTests(pages);
    }
    void SetHighRatioCommitPageLimitForTests(std::size_t pages) noexcept {
        for (auto& channel : m_highRatioStorage) channel.SetCommitPageLimitForTests(pages);
    }
    std::size_t ProfessionalStorageBytes() const noexcept { return m_professionalStorage.RequestedBytes(); }
    std::size_t ProfessionalDirectoryBytes() const noexcept { return m_professionalStorage.DirectoryBytes(); }
    void SetVariableWeightCommitPageLimitForTests(std::size_t pages) noexcept {
        m_variableWeightStorage.SetCommitPageLimitForTests(pages);
    }
    std::size_t VariableWeightStorageBytes() const noexcept { return m_variableWeightStorage.RequestedBytes(); }
    std::size_t VariableWeightDirectoryBytes() const noexcept { return m_variableWeightStorage.DirectoryBytes(); }
    std::uint32_t BlockFrames() const { return m_blockFrames; }

    // Records a voice admission refused with VN_NO_VOICE outside
    // Play()/CheckCapacityForPlay: logic-side policy, Logical Voice identity
    // exhaustion, or controlled logic-side allocation failure. Keeps the
    // NoVoiceCount observability exact regardless of which path refused.
    // Logic thread only.
    void NoteNoVoiceRefusal() { ++m_noVoiceRefusals; }

    // Render-capacity probe with refusal accounting: VN_OK when a Free slot
    // exists, else VN_NO_VOICE with the no-voice counter incremented —
    // exactly the refusal Play() would have produced. Lets the logic side
    // order Render Voice capacity before stream-slot capacity while keeping
    // both refusals side-effect-free. Logic thread only; the observed slot
    // stays Free until this thread claims it (the audio thread only frees).
    VnResult CheckCapacityForPlay();

    // Validates a handle before its command is enqueued: VN_OK for a live
    // (claimed or rendering) voice, otherwise VN_STALE_HANDLE with the
    // stale counter incremented. Logic thread only.
    //
    // The verdict is ADVISORY: the voice can end on the audio thread the
    // instant after this returns (an inherent, benign check-then-act
    // window). The authoritative staleness handling is always the audio
    // side's apply path, where a command for a dead handle is a no-op.
    VnResult CheckLive(VnVoiceHandle handle);

    // Observes identity lifetime without accessing the render payload. Exclusive
    // installation/release states remain alive until generation retirement.
    // The audio thread may retire the voice immediately after this observation.
    bool IsAlive(VnVoiceHandle handle) const noexcept;

    // Command application, audio side, at block start. A handle that went
    // stale between enqueue and apply (the voice ended on its own) is a
    // benign no-op - the call was valid when it was made. sourceStartFrame
    // seeds the PCM read cursor (0 = the file start, the original behavior; a
    // parked voice returns at its virtual offset by carrying it here). Stream
    // voices use their separately activated stream timeline.
    void ApplyPlay(VnVoiceHandle handle, VnVoiceHandle sourceIdentity,
                   std::uint64_t sourceStartFrame,
                   std::uint64_t blockIndex);
    void ApplyTimelineGatedPlay(VnVoiceHandle handle,
                                VnVoiceHandle sourceIdentity,
                                std::uint64_t sourceStartFrame,
                                std::uint64_t packedArmToken,
                                std::uint64_t blockIndex);
    bool TimelineBindingValid(std::uint32_t renderSlot,
                              std::uint32_t renderGeneration) const noexcept;
    bool CanAppendTimelineEvents(std::uint32_t renderSlot,
                                 std::uint32_t renderGeneration,
                                 std::uint64_t quantumIndex,
                                 std::uint32_t eventCount) const noexcept;
    bool AppendTimelineEvent(std::uint32_t renderSlot,
                             std::uint32_t renderGeneration,
                             std::uint64_t quantumIndex,
                             const VoiceTimelineEvent& event) noexcept;
    // Audio-only, between BeginTimelineQuantum and worker dispatch. Reservations
    // occupy the original span immediately, including while their guards are pending.
    bool ReserveTimelineEvents(std::uint32_t renderSlot,
                               std::uint32_t renderGeneration,
                               std::uint64_t quantumIndex, std::uint32_t count,
                               VoiceTimelineReservation* out) noexcept;
    bool ReserveLaneTimelineEvents(std::uint32_t renderSlot,
                                   std::uint32_t renderGeneration,
                                   std::uint64_t quantumIndex,
                                   std::uint32_t count,
                                   VoiceTimelineReservation* out) noexcept;
    bool StageReservedTimelineEvent(const VoiceTimelineReservation& reservation,
                                    std::uint32_t index,
                                    const VoiceTimelineEvent& event,
                                    const FrontierEntryReceiptRef& entry,
                                    const FrontierDueOperationRef& operation) noexcept;
    bool StageReservedLaneTimelineEvent(const VoiceTimelineReservation& reservation,
                                       std::uint32_t index,
                                       const VoiceTimelineEvent& event) noexcept;
    bool ProveTimelineReservation(const VoiceTimelineReservation& reservation,
                                  VoiceTimelineReservationProof* out) noexcept;
    bool MatchesHeldLaneStart(std::uint32_t renderSlot, std::uint32_t renderGeneration,
        std::uint64_t quantum, const VoiceTimelineEvent& expected,
        const VoiceTimelineReservationProof& reservation,
        const TimelineArmTable::LaneAudioProof& arm,
        const VariableTimelineInitializerProof& initializer) noexcept;
    bool RejectTimelineReservation(const VoiceTimelineReservation& reservation) noexcept;
    static void ApplyProvenTimelineReservation(VoiceTimelineReservationProof& proof,
                                               std::uint64_t cutoffFrame = UINT64_MAX,
                                               AuthoredStopOperationIdentity cutoff = {}) noexcept;
    static void ApplyProvenLaneTimelineReservation(
        VoiceTimelineReservationProof& proof) noexcept;
    static void RejectProvenTimelineReservation(
        VoiceTimelineReservationProof& proof) noexcept;
    static void ReleaseTimelineReservationProof(
        VoiceTimelineReservationProof& proof) noexcept { proof.Release(); }
    bool CancelTimelineBinding(std::uint32_t renderSlot,
                               std::uint32_t renderGeneration) noexcept;
    bool AdmitTimelineQuantum(std::uint32_t arenaAvailableBytes,
                              std::uint32_t blockFrames) const noexcept;
    bool AdmitTimelineQuantum(
        std::uint32_t arenaAvailableBytes, std::uint32_t blockFrames,
        const DecisionTable* decisions,
        const Plan03ScratchDecisionState* currentSlices,
        std::uint32_t currentSliceCount,
        const std::uint32_t* controlRenderGenerations = nullptr,
        std::uint32_t controlRenderCount = 0) const noexcept;
    std::uint64_t ScratchBytesBeforeTimeline(
        std::uint32_t blockFrames) const noexcept;
    std::uint64_t TimelineScratchBytes(std::uint32_t blockFrames) const noexcept;
    void BeginTimelineQuantum(std::uint64_t quantumIndex) noexcept;
    void FinalizeTimelineQuantum(std::uint64_t quantumIndex) noexcept;
    bool AbortClaimedVoice(VnVoiceHandle handle) noexcept;
    bool CommitPreparedVoice(VnVoiceHandle handle) noexcept;
    bool CancelPreparedVoice(VnVoiceHandle handle) noexcept;
    bool BindCompletionProgress(VnVoiceHandle handle,
                                CompletionProgressRow* row,
                                OccurrenceToken occurrence,
                                std::uint64_t playbackFactsRevision) noexcept;
    // fadeFrames takes precedence over fadeMs when nonzero (a steal declick
    // bounds its fade to one block in frames, immune to block-size rounding);
    // both zero is an immediate cut.
    void ApplyStop(VnVoiceHandle handle, std::uint32_t fadeMs,
                   std::uint32_t fadeFrames, NotificationQueue& notifications,
                   std::uint64_t blockIndex,
                   std::uint32_t mode = kStopModeOrdinary,
                   VnVoiceHandle publicHandle = 0,
                   std::uint64_t parkRequestSequence = 0);
    bool ReserveStreamPark(VnVoiceHandle renderHandle,
                           VnVoiceHandle publicHandle,
                           std::uint64_t requestSequence,
                           ResampleProfile profile =
                               ResampleProfile::LegacyCompatibility);
    void AbortStreamParkReservation(VnVoiceHandle renderHandle,
                                    VnVoiceHandle publicHandle,
                                    std::uint64_t requestSequence);
    StreamParkCancelResult CancelStreamPark(
        VnVoiceHandle renderHandle, VnVoiceHandle publicHandle,
        std::uint64_t requestSequence);
    bool ConsumeStreamParkOutcome(std::uint32_t renderSlot,
                                  StreamParkOutcome* out,
                                  bool* outCancelled);
    bool ConsumeStreamParkOutcomeExact(
        VnVoiceHandle renderHandle, VnVoiceHandle publicHandle,
        std::uint64_t requestSequence, StreamParkOutcome* out,
        bool* outCancelled);
    // Logic-owned reservations remain pending through cancellation and audio publication.
    bool HasPendingStreamParkMailboxes() const noexcept {
        return m_pendingStreamParkMailboxes != 0;
    }
    void DiscardAllStreamParkMailboxes();
    const StreamParkToken* ResolveParkOwnedToken(
        StreamParkTokenRef ref) const;
    void ReleaseParkOwnedToken(StreamParkTokenRef ref);
    void BeginAudioBlock(std::uint64_t blockIndex = 0);
    void EndAudioBlock() noexcept;
    void StageCompletionProgressForSilentBlock(
        std::uint64_t blockIndex) noexcept;
    bool PublishCompletionProgress(std::uint64_t blockIndex) noexcept;
    void AdvanceArenaSilentStreamParks(std::uint32_t blockFrames);

    // Voice parameter application, audio side, at block start. Values
    // arrive fully resolved from the logic-side combine model; these
    // methods only translate them into render state. All are benign
    // no-ops on a handle that went stale in flight.
    void ApplySetVolume(VnVoiceHandle handle, float volumeDb,
                        std::uint32_t rampMs, bool outputOnly = false);
    bool ApplyVariableWeightSegment(
        VnVoiceHandle handle,
        const WeightSegmentV1& segment) noexcept;
    bool ApplyOwnedVariableWeightSegment(VnVoiceHandle handle,
        VariableWeightControlIdentity owner, const WeightSegmentV1& segment) noexcept;
    bool PrepareVariableWeightControls(VnVoiceHandle handle,
        const VariableWeightControlBinding* bindings, std::uint32_t count) noexcept;
    bool RemoveOwnedVariableWeightSegment(VnVoiceHandle handle,
        VariableWeightControlIdentity owner, std::uint32_t expectedGeneration) noexcept;
    void ApplySetPitch(VnVoiceHandle handle, double ratio, std::uint32_t rampMs);
    void ApplySetFilter(VnVoiceHandle handle, std::uint32_t filterKind,
                        bool enable, float frequencyHz, float q,
                        const BiquadCoeffs& coeffs,
                        std::uint32_t tailFrames,
                        std::uint32_t transitionFrames);
    void ApplySetPosition(VnVoiceHandle handle, float x, float y, float z);
    void ApplySetPan(VnVoiceHandle handle, float pan, float gain0, float gain1,
                     std::uint32_t channel0, std::uint32_t channel1, float pan2d);
    void ApplySetPan(VnVoiceHandle handle, float pan, float gain0, float gain1,
                     std::uint32_t channel0, std::uint32_t channel1) {
        ApplySetPan(handle, pan, gain0, gain1, channel0, channel1, pan);
    }
    void ApplySetWidth(VnVoiceHandle handle, float width);
    void ApplySetSpatializationMix(VnVoiceHandle handle, float mix);
    void ApplySetShelf(VnVoiceHandle handle, std::uint32_t shelfKind,
                       bool enable, float frequencyHz, float q, float gainDb,
                       const BiquadCoeffs& coeffs,
                       std::uint32_t tailFrames,
                       std::uint32_t transitionFrames);
    void ApplySetLowShelf(VnVoiceHandle handle, float gainDb,
                          const BiquadCoeffs& coeffs,
                          std::uint32_t tailFrames) {
        ApplySetShelf(handle, kShelfKindLow, gainDb != 0.0f, 250.0f,
                      0.70710678118654752440f, gainDb, coeffs, tailFrames,
                      BiquadSmoothingFrames(m_sampleRate));
    }
    // Arms a per-slot bypass toggle: bypass != 0 blends the slot toward dry,
    // 0 toward wet, crossfading over the next block. slot is re-checked
    // against the voice's active chain length; a stale handle or an
    // out-of-range slot is a benign no-op (the logic side already validated
    // the slot, so this only guards a handle that died in flight).
    void ApplySetVoiceEffectBypass(VnVoiceHandle handle, std::uint32_t slot,
                                   std::uint32_t bypass,
                                   std::uint32_t action = kEffectActionBypass,
                                   std::uint32_t expectedEffectType =
                                       kEffectTypeNone);

    // Installs a pooled effect instance into a chain slot, retiring whatever
    // the slot held (an in-place replace) so the logic side reclaims it. The
    // instance was acquired and configured logic-side; here the audio thread
    // only resolves (effectType, poolIndex) to the pooled state and points the
    // slot at it. A handle that went stale in flight retires the new instance
    // (it was never installed) so it is never leaked. The slot keeps its
    // settled bypass mix, so installing into a bypassed slot is dry at once.
    void ApplySetVoiceEffect(VnVoiceHandle handle, std::uint32_t slot,
                             std::uint32_t effectType, std::uint32_t poolIndex,
                             std::uint64_t poolGeneration);
    // Removes a slot's effect at the apply boundary: the instance is retired
    // (its tail is cut, not drained), the slot becomes empty, and any
    // in-progress bypass ramp settles immediately to its target — so the slot
    // behaves exactly like one that was never occupied. Benign on a stale
    // handle or an out-of-range slot.
    void ApplyClearVoiceEffect(VnVoiceHandle handle, std::uint32_t slot);
    bool ApplyOwnedVoiceEffect(VnVoiceHandle handle, std::uint32_t slot,
        const EffectInstanceRef& incoming);
    bool ApplyOwnedClearVoiceEffect(VnVoiceHandle handle, std::uint32_t slot);
    // Applies one parameter to a slot's effect (block-boundary step). Tone
    // coefficients arrive prepared; other effect types use their ordinary
    // parameter callback. A no-op when the slot is empty, the handle went
    // stale in flight, or the slot now holds a different effect type.
    void ApplySetVoiceEffectParam(VnVoiceHandle handle, std::uint32_t slot,
                                  std::uint32_t paramIndex, float value,
                                  std::uint32_t effectType,
                                  const BiquadCoeffs& coeffs,
                                  std::uint32_t tailFrames,
                                  std::uint32_t transitionFrames = 0);
    void ApplySetVoiceCompressorParameters(
        VnVoiceHandle handle, std::uint32_t slot,
        std::uint32_t effectType,
        const CompressorPreparedParameters& prepared,
        std::uint32_t transitionFrames);

    // Swap-time bus tombstone scrub (the destroy cascade): every rendering
    // voice whose dry bus died migrates to its nearest alive ancestor through
    // the plan's parents (the master is always alive, so the walk terminates),
    // and every voice send slot whose target died deactivates. Claimed voices
    // remain logic-thread-owned until their play command publishes them and
    // are therefore left untouched. Runs in the same block as the plan swap
    // that tombstoned the slots, so no rendered voice folds into a dead slot's
    // planes. Audio thread.
    void ApplyBusTombstones(const std::uint8_t* alive,
                            const std::uint32_t* parents,
                            std::uint32_t slotCount,
                            const std::uint32_t* generations = nullptr,
                            const std::uint32_t* migrationTargets = nullptr);

    // Voice->bus send application, audio side, at block start. setVoiceSend
    // installs (or replaces) a send slot's target bus and snaps its initial
    // level; clearVoiceSend deactivates the slot; setVoiceSendLevel ramps an
    // active slot's level with the multi-block bus-volume discipline. The
    // sendSlot index and target bus were validated on the logic side; a stale
    // handle or an out-of-range slot is a benign no-op.
    void ApplySetVoiceSend(VnVoiceHandle handle, std::uint32_t sendSlot,
                           std::uint32_t targetBusIndex,
                           float levelDb,
                           std::uint32_t targetBusGeneration = 0);
    void ApplyClearVoiceSend(VnVoiceHandle handle, std::uint32_t sendSlot);
    void ApplySetVoiceSendLevel(VnVoiceHandle handle, std::uint32_t sendSlot,
                                float levelDb, std::uint32_t rampMs);

    struct RenderAllResult {
        std::uint64_t overrunTicks = 0;
        std::uint32_t pendingVoices = 0;
        bool batchDeferred = false;
    };

    // A deferred quantum owns all its original payload until the last writer
    // completes. Lifecycle Stop establishes the same fact by joining.
    bool DeferredRenderReady() const noexcept {
        return m_workers.PendingJobsCompleted();
    }

    // Renders every rendering voice into its bus's planar accumulator and
    // releases the ones that finished, emitting their notifications.
    // `busPlanes` is bus-major: bus i's channels start at
    // busPlanes[i * mixChannels] (the resolved mix-layout channel count).
    //
    // jobDeadlineAt is the absolute cutoff; see RenderWorkerPool::RunUntil.
    // 0 renders without a bound, which only a caller that owns no deadline may
    // ask for. Audio thread.
    RenderAllResult RenderAll(BlockArena& arena, float* const* busPlanes,
                              std::uint32_t blockFrames,
                              NotificationQueue& notifications,
                              std::uint64_t blockIndex,
                              std::uint64_t jobDeadlineAt,
                              std::uint64_t sourceFrameBegin = 0);

    // Publishes a valid all-zero current block for every live Voice tap when
    // the mixer cannot allocate the Bus planes and therefore cannot enter
    // RenderAll. This preserves liveness and previous-block timing without
    // advancing Voice DSP or lifecycle state. Audio thread.
    void PublishLiveRouteSilence();

    // Fills the payload's voice entries (first VN_SNAPSHOT_MAX_VOICES
    // slots, indexed by slot) and the active-voice total. Audio thread,
    // at the end of a rendered block. Only Rendering slots contribute —
    // the snapshot describes the rendered block, and only Rendering
    // contexts are audio-owned and safe to read here; a Claimed voice
    // (play still in flight) reports INACTIVE until its first block
    // renders. Entries beyond the pool's capacity are zeroed so a reused
    // payload half never leaks stale slots.
    void GatherSnapshot(SnapshotPayload& out) const;
    std::uint32_t RenderedState(VnVoiceHandle handle) const noexcept;

    // Fills the source-cursor mirror rows (one per slot, up to count): a
    // Rendering slot reports its current PCM source read cursor and active =
    // true, every other slot reports inactive. Audio thread, at the end of a
    // rendered block. Stream slots report their (unused-for-park) srcCursor;
    // audibility parking consults only PCM voices.
    void FillSourceCursors(SourceCursorMirror::Row* rows,
                           std::uint32_t count) const;

    // Audio-thread resident-source seek transaction. CanApplySeek is read-only
    // so MixerLoop can prove a complete batch before CommitSeek mutates any
    // render context.
    VnResult CanApplySeek(VnVoiceHandle handle, std::uint32_t mode,
                          std::int64_t frameValue,
                          std::uint64_t* outSourceFrame,
                          const StreamSeekHandoffSnapshot* stream = nullptr) noexcept;
    void CommitSeek(VnVoiceHandle handle,
                    std::uint64_t sourceFrame,
                    const StreamSeekHandoffSnapshot* stream = nullptr) noexcept;

    // Resolves a handle to its live slot; nullptr when the handle is stale.
    // Internal surface (host/tests); public operations go through play/stop.
    // On the logic thread the pointer may be used only as a liveness
    // predicate (advisory, like CheckLive) — dereferencing render state
    // that the audio thread mutates is out of contract there. Single-
    // threaded harnesses (null backend pumped by the caller) may inspect
    // the context freely.
    VoiceRenderContext* Resolve(VnVoiceHandle handle);

    // Audio-command-local ownership of the current claimed physical source.
    // The held Core proof stays in stable storage until Finish or untouched Release.
    class SourceCommandInstallProof {
    public:
        SourceCommandInstallProof() = default;
        SourceCommandInstallProof(const SourceCommandInstallProof&) = delete;
        SourceCommandInstallProof& operator=(const SourceCommandInstallProof&) = delete;
        SourceCommandInstallProof(SourceCommandInstallProof&& other) noexcept;
        SourceCommandInstallProof& operator=(SourceCommandInstallProof&& other) noexcept;
        ~SourceCommandInstallProof() { Release(); }
        bool Valid() const noexcept { return slot_ != nullptr; }
        VoiceReleaseObserver ReleaseObserver() const noexcept { return release_; }
        void Release() noexcept;
    private:
        friend class VoicePool;
        VoicePool* pool_ = nullptr;
        Slot* slot_ = nullptr;
        const TimelineArmTable::SourceCommandRowProof* source_ = nullptr;
        VoiceReleaseObserver release_{};
        std::uint64_t block_ = 0;
        std::uint64_t originalBodyMarker_ = 0;
        bool begun_ = false;
    };
    // Only audio metadata dispatch uses this actual proof; ordinary Resolve and
    // every physical release path still refuse CommandInstallHeld.
    class SourceCommandMetadataAccess {
    public:
        explicit SourceCommandMetadataAccess(SourceCommandInstallProof& proof,
            bool enabled = true) noexcept;
        SourceCommandMetadataAccess(const SourceCommandMetadataAccess&) = delete;
        SourceCommandMetadataAccess& operator=(const SourceCommandMetadataAccess&) = delete;
        ~SourceCommandMetadataAccess() { Release(); }
        void Release() noexcept;
    private:
        VoicePool* pool_ = nullptr;
    };
    enum class SourceCommandInstallStatus : std::uint8_t {
        Ready, DeferredInstallation, NotClaimed
    };
    class OriginalVoiceOwner;
    SourceCommandInstallStatus ProveSourceCommandInstallation(
        const TimelineArmTable::SourceCommandRowProof& source,
        SourceCommandInstallProof* out, const OriginalVoiceOwner* original = nullptr) noexcept;
    static void BeginSourceCommandInstallation(SourceCommandInstallProof& proof,
        VnVoiceHandle sourceIdentity, std::uint64_t sourceStartFrame,
        std::uint64_t blockIndex) noexcept;
    static void FinishSourceCommandInstallation(SourceCommandInstallProof& proof) noexcept;

    struct PreparedRoutingV1 {
        struct Send {
            bool active = false;
            std::uint32_t busIndex = 0;
            std::uint32_t busGeneration = 0;
            bool operator==(const Send&) const noexcept = default;
        };
        std::uint32_t busIndex = 0;
        std::uint32_t busGeneration = 0;
        std::array<Send, kMaxVoiceSendSlots> sends{};
        bool operator==(const PreparedRoutingV1&) const noexcept = default;
    };
    struct PreparedCommitInput {
        VnVoiceHandle renderHandle = 0;
        VoiceReleaseObserver originalRelease{};
        SourceTerminalRecord* sourceTerminal = nullptr;
        SourceTerminalToken sourceTerminalToken{};
        CompletionProgressRow* completionProgress = nullptr;
        OccurrenceToken completionOccurrence{};
        std::uint32_t completionRenderGeneration = 0;
        std::uint64_t playbackFactsRevision = 0;
        StreamRing* streamRing = nullptr;
        std::uint64_t streamRingToken = 0;
        std::uint32_t streamGeneration = 0;
        PreparedRoutingV1 routing{};
    };
    // The pool must outlive this exclusive right to its original Prepared slot.
    class PreparedCommitProof {
    public:
        PreparedCommitProof() = default;
        PreparedCommitProof(const PreparedCommitProof&) = delete;
        PreparedCommitProof& operator=(const PreparedCommitProof&) = delete;
        PreparedCommitProof(PreparedCommitProof&& other) noexcept;
        PreparedCommitProof& operator=(PreparedCommitProof&& other) noexcept;
        ~PreparedCommitProof();
        bool Valid() const noexcept { return slot_ != nullptr; }
        const VoicePool* Owner() const noexcept { return pool_; }
        VnVoiceHandle OriginalPreparedHandle() const noexcept;
        void Release() noexcept;
    private:
        friend class VoicePool;
        VoicePool* pool_ = nullptr;
        Slot* slot_ = nullptr;
    };
    bool CapturePreparedCompletionProgress(const PreparedCommitProof& proof,
        const CompletionProgressRow** progress, OccurrenceToken* occurrence) const noexcept;
    bool ProvePreparedCommit(const PreparedCommitInput& expected,
                             PreparedCommitProof* out) noexcept;
    static void ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept;

    // Historical physical origin issued by a genuine exclusive preparation.
    // The pool must outlive this owner. Dropping it never releases a voice.
    class OriginalVoiceOwner {
    public:
        OriginalVoiceOwner() = default;
        OriginalVoiceOwner(const OriginalVoiceOwner&) = delete;
        OriginalVoiceOwner& operator=(const OriginalVoiceOwner&) = delete;
        OriginalVoiceOwner(OriginalVoiceOwner&& other) noexcept;
        OriginalVoiceOwner& operator=(OriginalVoiceOwner&& other) noexcept;
        ~OriginalVoiceOwner() { Release(); }
        bool Valid() const noexcept { return pool_ != nullptr; }
        const VoicePool* Owner() const noexcept { return pool_; }
        VnVoiceHandle RenderHandle() const noexcept { return handle_; }
        void Release() noexcept;
    private:
        friend class VoicePool;
        VoicePool* pool_ = nullptr;
        VnVoiceHandle handle_ = 0;
        std::uint64_t releaseTarget_ = 0;
    };
    // Logic thread is the sole issuer; registrations may retire independently.
    bool IssueOriginalVoiceOwner(const PreparedCommitProof& preparation,
        OriginalVoiceOwner* out) noexcept;
    enum class OriginalVoiceDisposal : std::uint8_t {
        Released, AlreadyReleased, Deferred, Refused
    };
    // Audio coordinator only, with the existing worker/read-proof quiescence.
    // This is physical disposal, not Core/Frontier/command-body retirement.
    OriginalVoiceDisposal TryDisposeOriginalTimelineVoice(
        const OriginalVoiceOwner& owner) noexcept;
    bool OriginalVoiceReleased(const OriginalVoiceOwner& owner) const noexcept;
    bool CancelOriginalPreparedVoice(const OriginalVoiceOwner& owner) noexcept;
    bool MatchesOriginalPreparedVoice(const OriginalVoiceOwner& original,
        const PreparedCommitProof& proof) const noexcept;
    bool CapturePreparedRoutingV1(VnVoiceHandle handle, PreparedRoutingV1* out) const noexcept;
    bool CanCommitPreparedRoutingMigrationV1(VnVoiceHandle handle,
        const PreparedRoutingV1& expected, const PreparedRoutingV1& next) const noexcept;
    void CommitPreparedRoutingMigrationV1(VnVoiceHandle handle,
        const PreparedRoutingV1& expected, const PreparedRoutingV1& next) noexcept;

    // This authentic component preserves one physical Slot storage episode.
    // It grants no render/PCM reader, completion, Decision or Queue authority.
    class OriginalSourceBodyLifetimePin {
    public:
        OriginalSourceBodyLifetimePin() noexcept = default;
        OriginalSourceBodyLifetimePin(const OriginalSourceBodyLifetimePin&) = delete;
        OriginalSourceBodyLifetimePin& operator=(const OriginalSourceBodyLifetimePin&) = delete;
        OriginalSourceBodyLifetimePin(OriginalSourceBodyLifetimePin&& other) noexcept;
        OriginalSourceBodyLifetimePin& operator=(OriginalSourceBodyLifetimePin&& other) noexcept;
        ~OriginalSourceBodyLifetimePin() { Release(); }
        bool Valid() const noexcept { return pool_ != nullptr; }
        bool Borrow(OriginalSourceBodyLifetimePin* out) const noexcept;
        void Release() noexcept;
    private:
        friend class VoicePool;
        VoicePool* pool_ = nullptr;
        std::uint32_t slot_ = UINT32_MAX;
        TimelineArmToken arm_{};
        std::uint64_t marker_ = 0;
    };
    bool IsOriginalSourceBodyLifetimePinFor(const OriginalSourceBodyLifetimePin& original,
        TimelineArmToken arm, VnVoiceHandle sourceIdentity) const noexcept;
    bool IsOriginalSourceBodyCurrent(const OriginalSourceBodyLifetimePin& original) const noexcept;
    // Lifecycle thread only, after callback stop. This performs the real worker
    // join before returning any quarantined installation loan. Not an audio fence.
    void ShutdownOriginalSourceBodiesAfterQuiescence() noexcept;

    std::uint32_t ActiveCount() const;
    std::uint64_t StaleHandleCount() const { return m_staleHandleHits; }
    std::uint64_t InvalidVolumeApplyCount() const {
        return m_invalidVolumeApplies;
    }
    std::uint64_t NoVoiceCount() const { return m_noVoiceRefusals; }
    std::uint64_t CompletedCount() const { return m_completed; }
    std::uint32_t HighWaterVoices() const { return m_highWater; }
    // STARVED notifications emitted (cooldown-aligned, not per starved block).
    // Audio-thread-owned; published to the logic side via the snapshot.
    std::uint64_t VoiceStarvedCount() const { return m_voiceStarved; }
    std::uint32_t StreamParkCaptureBytesThisBlock() const {
        return m_streamParkCaptureBytesThisBlock;
    }
    std::uint32_t StreamParkCaptureBytesPeak() const {
        return m_streamParkCaptureBytesPeak;
    }

    // Active render-worker count (0 = serial in-callback rendering). Fixed at
    // init from the resolved config.
    std::uint32_t RenderWorkerCount() const { return m_workers.WorkerCount(); }

    // Lifecycle-thread recovery for the init-time fixed worker count. A
    // healthy disabled set is reused; a set force-stopped by a prior platform
    // failure is fully recreated. This allocates and must run before any
    // callback is admitted.
    [[nodiscard]] bool EnsureRenderWorkersReady() noexcept {
        return m_workers.EnsureReady();
    }

    // Real-device lifecycle bridge. The backend starts and validates its
    // callback while render admission remains closed, binds every worker to
    // the callback's published scheduling domain, then enables dispatch.
    // Shutdown closes admission and disables workers first, synchronously
    // stops the callback, and only then unbinds.
    bool BindRealtimeWorkerDomain(const RealtimeWorkerDomain& domain) {
        return m_workers.BindRealtimeDomain(domain);
    }
    bool UnbindRealtimeWorkerDomain() {
        return m_workers.UnbindRealtimeDomain();
    }
    bool EnableRealtimeRenderWorkers() noexcept {
        return m_workers.EnableRealtime();
    }
    bool EnableOfflineRenderWorkers() noexcept {
        return m_workers.EnableOffline();
    }
    void DisableRenderWorkers() noexcept { m_workers.Disable(); }

    // Peak per-block worker busy span since init, in performance-counter ticks
    // (0 when serial or never loaded). The mixer converts this to the
    // snapshot's renderWorkerBusyPeakUs. Audio-thread-owned: written in
    // RenderAll and read by the mixer's publish on the same thread.
    std::uint64_t RenderWorkerBusyPeakTicks() const {
        return m_workerBusyPeakTicks;
    }
    std::uint64_t AudioCompressorDetectorNonFiniteCount() const {
        return m_effectBank.AudioCompressorDetectorNonFiniteCount();
    }
    std::uint64_t AudioCompressorDetectorSaturationCount() const {
        return m_effectBank.AudioCompressorDetectorSaturationCount();
    }

private:
    friend struct detail::VoicePoolReleaseTestAccess;
    // Free -> Claimed on logic-side play; Claimed -> Rendering when the
    // audio side applies the play command; back to Free on release.
    //
    // Quarantined is terminal and has no transition out of it, which is how
    // "this slot is never handed out again" is expressed structurally rather
    // than as a rule someone has to remember. A slot lands there when the
    // render deadline expired while a worker still held its cascade: that worker
    // may still be writing the slot's render context and scratch, and nothing
    // in this design ever proves it stopped, so the slot, its pooled effect
    // instances, its PCM reference and its stream slot are all held for good.
    enum class SlotState : std::uint8_t {
        Free,
        Claimed,
        VariableStartClaimed,
        Rendering,
        VariableStartWaiting,
        OriginalCallbackHeld,
        TimelineCallbackHeld,
        Quarantined,
        Prepared,
        PreparedCommitHeld,
        CommandInstallHeld,
        ReleaseHeld,
    };

    // The two liveness fields are read by the logic thread (handle
    // validation, free-slot scan) while the audio thread writes them
    // (release, play application), so they are atomics. The render
    // context itself is NOT protected by them alone: ownership hands over
    // through release/acquire on `state` (audio's final render writes ->
    // state Free -> logic re-init) and through the command queue (logic's
    // context build -> play command -> audio rendering). Slot storage is
    // sized once at construction and never copied or moved afterwards
    // (atomics are neither copyable nor movable).
    static bool IsAudioRenderingState(SlotState state) noexcept {
        return state == SlotState::Rendering || state == SlotState::OriginalCallbackHeld ||
            state == SlotState::TimelineCallbackHeld;
    }

    enum class FirstOutputPhase : std::uint8_t { Prepared, AwaitingFirstFold, FirstFolded, Live };
    struct DeferredFirstOutputEndpoint {
        bool valid = false;
        VariablePlaybackOrder order{};
        std::uint32_t targetRevision = 0;
        VariableNativeEffectIdentity nativeEffect{};
        VariableToneBandValues toneBand{};
        CommandRecord command{};
    };
    struct DeferredFirstOutputWeight {
        bool valid = false;
        VariablePlaybackOrder order{};
        VariableWeightControlIdentity weightOwner{};
        WeightSegmentV1 weightSegment{};
        bool weightRemoved = false;
    };
    struct alignas(64) Slot {
        std::atomic<SlotState> state{SlotState::Free};
        std::atomic<std::uint32_t> generation{1};
        std::atomic<std::uint64_t> releaseSequence{0};
        std::uint64_t issuedOriginalReleaseTarget = 0;
        std::atomic<std::uint64_t> originalBodyLifetime{0};
        std::atomic<std::uint64_t> originalBodyRetirementRequest{0};
        std::uint64_t originalBodyMarker = 0;
        TimelineArmToken originalBodyArm{};
        VnVoiceHandle originalBodySourceIdentity = 0;
        std::uint64_t originalBodyReleaseTarget = 0;
        std::atomic<bool> originalBodyInstallationCompleted{false};
        OriginalSourceInstallationLoan pendingOriginalSource;
        OriginalSourceInstallationLoan installedOriginalSource;
        std::atomic<VnVoiceHandle> monitorPublicHandle{0};
        MonitorVoiceControlBinding monitorControl{};
        bool monitorSourceTerminalPublished = false;
        std::atomic<std::uint64_t> streamRingToken{0};
        mutable std::atomic<std::uint64_t> renderedState{0};
        VoiceRenderContext render;
        VariableNativeStartIdentity nativeVariableStart{};
        VariableTimelineReadUseRef timelineVariableReadUse{};
        bool firstOutputOrderFinal = true;
        VariablePlaybackOrder firstOutputOrder{};
        FirstOutputPhase firstOutputPhase = FirstOutputPhase::Live;
        CompletionProgressRow* stagedCompletionProgress = nullptr;
        CompletionProgressBlockUpdate stagedCompletionUpdate{};
        bool completionProgressStaged = false;
        bool completionProgressTerminal = false;
        std::uint64_t timelineReservationEpoch = 0;
        std::uint32_t timelineReservedMask = 0;
        std::uint32_t timelineFilledMask = 0;
        std::uint32_t timelineProvenMask = 0;
        std::uint32_t timelinePageEventCount = 0;
        std::uint32_t timelineLaneEventCount = 0;
        std::uint8_t timelineReservationCounts[kTimelineMaxCombinedEventsPerVoiceQuantum]{};
        // An odd cache-line stride spreads hot fields across cache sets.
        std::uint64_t cacheSetSpacer[7] = {};
    };
    VariableNativeStartAuthority m_nativeVariableStartAuthority{};
    VariableTimelineStartAuthority m_timelineVariableStartAuthority{};
    bool m_timelineQuantumOpen = false;
    void ReturnTimelineCallbackStorage() noexcept;
    bool FinishNativeVariableStart(Slot& slot, bool activated) noexcept;
    static constexpr std::uint32_t kFirstOutputBuiltinEndpoints = 11;
    std::uint32_t m_firstOutputStride = 0;
    StableRowStorage<DeferredFirstOutputEndpoint> m_firstOutputEndpoints;
    StableRowStorage<DeferredFirstOutputWeight> m_firstOutputWeights;
    StableRowStorage<std::array<VoiceTimelineEvent, kTimelineMaxEventsPerVoiceQuantum>>
        m_pendingTimelineVariableEvents;
    bool FirstOutputBackingReady(std::uint32_t count) const noexcept;
    bool PrepareFirstOutputBacking(std::uint32_t count) noexcept;
    void* m_firstOutputCommandContext = nullptr;
    void (*m_firstOutputCommandConsumer)(void*, const CommandRecord&) noexcept = nullptr;
    void ClearFirstOutput(Slot& slot) noexcept;
    std::uint32_t FirstOutputCommandKey(const CommandRecord& command,
        VnVoiceHandle& handle) const noexcept;
    void SupersedeDeferredFirstOutput(const CommandRecord& command,
        VnVoiceHandle initializingVoice = 0) noexcept;
    void ApplyDeferredFirstOutput(Slot& slot) noexcept;
    DeferredFirstOutputWeight* FindFirstOutputWeightEndpoint(Slot& slot,
        VariableWeightControlIdentity owner, bool create) noexcept;
    void PrepareFirstOutputTimelineWeights(Slot& slot) noexcept;
    static bool FirstOutputProtected(const Slot& slot) noexcept;

    Slot* ResolveTimelineReservation(const VoiceTimelineReservation& reservation) noexcept;

    friend class OriginalSourceSubmissionRegistry;
    friend class VoiceTimelineSink;
    friend class MixerLoop;
    // Audio-only cleanup preserves the real callback reader until EndAudioBlock.
    bool CancelTimelineBindingFromAudio(std::uint32_t renderSlot,
        std::uint32_t renderGeneration) noexcept;
    static constexpr std::uint64_t kOriginalBodyPinMask = UINT32_MAX;
    static constexpr std::uint64_t kOriginalBodyDisposed = UINT64_C(1) << 63;
    static constexpr std::uint64_t kOriginalBodyLoanReturned = UINT64_C(1) << 62;
    static constexpr std::uint64_t kOriginalBodyReturnClaimed = UINT64_C(1) << 61;
    static constexpr std::uint64_t kOriginalBodyCallbackReader = UINT64_C(1) << 60;
    bool CanIssueOriginalSourceBody(const PreparedCommitProof& proof,
        const OriginalSourceBodyLifetimePin* out) const noexcept;
    void IssueOriginalSourceBody(const PreparedCommitProof& proof,
        TimelineArmToken arm, VnVoiceHandle sourceIdentity,
        OriginalSourceInstallationLoan&& installation,
        OriginalSourceBodyLifetimePin* out) noexcept;
    bool MatchesPreparedOriginalSourceBody(const OriginalSourceBodyLifetimePin& original,
        const PreparedCommitProof& preparation, const PreparedCommitInput& expected,
        const OriginalSourceInstallationLoan** actualPending) const noexcept;
    bool MatchesPreparedOriginalSourceStorage(const OriginalSourceBodyLifetimePin& original,
        const PreparedCommitProof& preparation, const PreparedCommitInput& expected,
        const OriginalSourceInstallationLoan** actualPending) const noexcept;
    bool MatchesOriginalSourceBody(const OriginalSourceBodyLifetimePin& original) const noexcept;
    bool MatchesOriginalSourceRetirementPair(const OriginalVoiceOwner& owner,
        const OriginalSourceBodyLifetimePin& body) const noexcept;
    bool CaptureOriginalSourceRetirementIdentity(const OriginalVoiceOwner& owner,
        const OriginalSourceBodyLifetimePin& body, VoiceReleaseObserver* release,
        SourceTerminalToken* terminal) const noexcept;
    bool CanReturnUnpublishedOriginalSourceBodyStorage(const OriginalSourceBodyLifetimePin& body,
        const OriginalSourceInstallationLoan** installation) const noexcept;
    bool CanReturnPublishedOriginalSourceBodyStorage(const OriginalVoiceOwner& owner,
        const OriginalSourceBodyLifetimePin& body, VoiceReleaseObserver release,
        const OriginalSourceInstallationLoan** installation) const noexcept;
    void RequestOriginalSourceBodyRetirement(const OriginalSourceBodyLifetimePin& body) noexcept;
    bool ConsumeOriginalSourceBodyRetirement(Slot& slot, std::uint64_t blockIndex) noexcept;
    bool BorrowOriginalSourceBody(const OriginalSourceBodyLifetimePin& original,
        OriginalSourceBodyLifetimePin* out) noexcept;
    void ReleaseOriginalSourceBody(OriginalSourceBodyLifetimePin& original) noexcept;
    struct OriginalCallbackStorageLease {
        OriginalSourceMutationActor actor;
        TimelineArmToken arm{};
        std::uint64_t bodyMarker = 0;
        bool storageHeld = false;
    };
    // Only actual audio-owned Rendering or a true InstallProof can enter.
    void AcquireOriginalCallbackStorage(Slot& slot,
        const SourceCommandInstallProof* installation = nullptr) noexcept;
    void ReturnOriginalCallbackStorage(Slot& slot, OriginalCallbackStorageLease& lease) noexcept;
    static void ReturnOriginalSourceInstallation(Slot& slot) noexcept;
    static void CompleteOriginalSourceBodyDisposition(Slot& slot) noexcept;
    static void ResetOriginalSourceBodyForReuse(Slot& slot) noexcept;
    static bool CanClaimSlot(const Slot& slot) noexcept;
    static void PublishRelease(Slot& slot) noexcept;

    enum class StreamParkTokenState : std::uint8_t {
        Free,
        TransactionOwned,
        Published,
        ParkOwned
    };
    static_assert(std::atomic<StreamParkTokenState>::is_always_lock_free,
                  "stream park token ownership must be lock-free");

    struct StreamParkTokenSlot {
        std::atomic<StreamParkTokenState> state{
            StreamParkTokenState::Free};
        std::uint32_t generation = 1;
        StreamParkToken payload{};
    };

    Slot* SlotFor(VnVoiceHandle handle);
    Slot* SlotForCommandMetadata(VnVoiceHandle handle);
    void InitializePlay(Slot& slot, VnVoiceHandle sourceIdentity,
        std::uint64_t sourceStartFrame, std::uint64_t blockIndex);
    StreamParkTokenRef ReserveStreamParkToken();
    StreamParkTokenSlot* ResolveStreamParkToken(
        StreamParkTokenRef ref, StreamParkTokenState expected);
    void ReleaseStreamParkToken(StreamParkTokenRef ref,
                                StreamParkTokenState expected);
    void ResetStreamParkCaptureBudget();
    void ReleaseSlot(Slot& slot);
    void PublishSourceTerminal(Slot& slot, std::uint64_t blockIndex) noexcept;
    void PublishPlaybackEnd(Slot& slot, std::uint64_t blockIndex) noexcept;
    bool StageCompletionProgress(Slot& slot,
                                 std::uint64_t blockIndex,
                                 std::uint32_t sourceOutputFrames) noexcept;
    void FinishStreamPark(std::uint32_t slotIndex, Slot* slot,
                          StreamParkDisposition disposition);
    void AdvanceStreamParkDeadline(std::uint32_t slotIndex, Slot& slot,
                                   std::uint32_t silentFrames);

    // Post-cascade lifecycle for one rendered voice: cooldown-spaced STARVED
    // notifications for a starved stream, and release + completion notification
    // on a Done phase. Shared by the serial and parallel render paths so both
    // emit identically, in slot order. Audio thread.
    void HandleVoiceLifecycle(std::uint32_t slotIndex, Slot& slot,
                              const StageResult& result,
                              NotificationQueue& notifications,
                              std::uint64_t blockIndex);

    // Puts one slot beyond reuse and reports its voice ended. Reads nothing a
    // stranded worker could be writing: the slot index came from the job list
    // the coordinator built before dispatch, and the generation is this thread's
    // own. In particular it does not read the render context to find the voice's
    // dynamic-route identity — see the tap discussion at the call site.
    void QuarantineSlot(std::uint32_t slotIndex, Slot& slot,
                        NotificationQueue& notifications,
                        std::uint64_t blockIndex);
    void NoteMonitorVoiceRetired(VnVoiceHandle publicHandle) noexcept;

    // Worker-pool cascade job: runs the cascade for the voice at job index j
    // (mapped through m_jobSlots), skipping a voice the pre-pass declined.
    // Reads/writes only that voice's context and scratch — worker-safe.
    void RunCascadeJob(std::uint32_t jobIndex);
    static void CascadeThunk(void* userData, std::uint32_t jobIndex);

    std::uint32_t m_sampleRate;
    std::uint32_t m_pitchRampFrames;
    std::uint32_t m_blockFrames;
    std::uint32_t m_fxSlotsPerVoice;
    std::uint32_t m_starvationCooldownBlocks;
    // Mix layout the fold-in pans/maps into (resolved config). Stereo at the
    // default, at which the fold-in takes the frozen two-channel law.
    std::uint32_t m_mixChannels;
    std::uint32_t m_mixLayout;
    const output_gain_detail::MonoStereoMixKernel m_monoStereoMix;
    const ResampleTables* m_tables;
    const MultirateResampleTables* m_multirateTables;
    const BiquadTrigTable* m_biquadTrig;
    EffectPoolBank& m_effectBank; // effect instance pools + retire channel
    DynamicRouteSystem& m_dynamicRoutes;
    std::atomic<void (*)(void*)> m_cancelTimelineReadHook{nullptr};
    std::atomic<void*> m_cancelTimelineReadContext{nullptr};
    std::atomic<void (*)(void*)> m_releaseHeldHookForTest{nullptr};
    std::atomic<void*> m_releaseHeldContextForTest{nullptr};
    TimelineArmTable* m_timelineArms = nullptr;
    const SourceCommandInstallProof* m_commandMetadataProof = nullptr;
    std::atomic<std::uint64_t> m_liveOriginalVoiceOwners{0};
    std::uint64_t m_nextOriginalBodyMarker = 1;
    bool m_originalBodyMarkerExhausted = false;
    bool m_originalBodyShutdown = false;
    StableRowStorage<Slot> m_slots;
    SlotStorageRegistration* m_slotStorageRegistrations = nullptr;
    std::vector<OriginalCallbackStorageLease> m_originalCallbackStorage;
    bool m_originalSourceCallbackOpen = false;
    std::vector<StreamParkMailbox> m_streamParkMailboxes;
    std::uint32_t m_pendingStreamParkMailboxes = 0;
    StableRowStorage<StreamParkTokenSlot> m_streamParkTokens;
    bool PrepareRenderSlotBacking(std::uint32_t count, std::uint32_t channelCount,
        bool firstOutput, std::uint32_t highRatioChannels = 0) noexcept;
    bool RenderSlotBackingReady(std::uint32_t count, std::uint32_t channelCount,
        std::uint32_t highRatioChannels) const noexcept;
    std::array<StableRowStorage<std::array<float, kResampleFifoFrames>>, kMaxChannels> m_ringStorage;
    std::array<StableRowStorage<ResamplerHighRatioChannelState>, kMaxChannels> m_highRatioStorage;
    ResamplerHighRatioColumns m_highRatioColumns{};
    void BindHighRatioBacking(std::uint32_t slot) noexcept;
    void BindMonitorPreChainBacking(std::uint32_t slot) noexcept;
    StableSlotStorage<MultirateResamplerCandidate> m_professionalStorage;
    using VariableWeightStorage = std::array<VoiceRenderContext::OwnedVariableWeight, kSelectionPathNodeCapacity>;
    StableSlotStorage<VariableWeightStorage> m_variableWeightStorage;
    bool PrepareVariableWeightBacking(std::uint32_t slot) noexcept;
    void BindVariableWeightBacking(std::uint32_t slot) noexcept;
    std::uint64_t m_staleHandleHits = 0;
    std::uint64_t m_invalidVolumeApplies = 0;
    std::uint64_t m_noVoiceRefusals = 0;
    std::uint64_t m_completed = 0;
    std::uint64_t m_voiceStarved = 0; // audio-thread-owned notification count
    std::uint32_t m_highWater = 0;
    static constexpr std::uint32_t kStreamParkCaptureBudgetBytes =
        2u * 1024u * 1024u;
    std::uint32_t m_streamParkCaptureBytesThisBlock = 0;
    std::uint32_t m_streamParkCaptureBytesPeak = 0;

    // Job-parallel render workers (0 = serial). The count is frozen at engine
    // init; the lifecycle thread may recreate that same fixed set after a full
    // fault teardown. Workers park between blocks. RenderAll fans independent
    // voice cascades across the pool and folds results single-threaded in slot
    // order (§8.2 determinism).
    RenderWorkerPool m_workers;
    // Per-block cascade job list, filled in slot order by the parallel path's
    // serial pre-pass: m_jobSlots[j] is the voice slot of job j. Sized once at
    // construction (Render capacity) so dispatch allocates nothing on the audio
    // path.
    std::vector<std::uint32_t> m_jobSlots;
    std::vector<std::uint32_t> m_jobGenerations;
    std::uint32_t m_deferredJobCount = 0;
    std::uint64_t m_jobQuantumIndex = 0;
    std::uint32_t m_jobBlockFrames = 0; // block width for the in-flight batch
    std::uint64_t m_jobSourceFrameBegin = 0;
    std::array<StableRowStorage<float>, kMaxChannels> m_monitorPreChainStorage;
    std::atomic<bool> m_monitorPreChainPrepared{false};
    std::uint64_t m_workerBusyPeakTicks = 0; // running peak, audio-thread-owned
};

} // namespace harken

#endif // HARKEN_RENDER_VOICE_POOL_H
