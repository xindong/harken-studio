#ifndef HARKEN_PARAM_VARIABLE_SERVICE_H
#define HARKEN_PARAM_VARIABLE_SERVICE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/CurrentWorld.h"
#include "harken/core/BankVersionToken.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/ActionFiringTable.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/core/VariableSliceTable.h"
#include "harken/core/VariableReadCut.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/VariableLabel.h"
#include "harken/render/OriginalSourceMutationActor.h"
#include "harken/param/ParamTargets.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace harken {

namespace detail {
struct VariableServiceTestAccess;
}

struct Engine;
struct EngineActionPlayScratch;
struct EngineImmediateEntryAttempt;
class BindingTable;
class BusTargetStore;
class CommandQueue;
class EffectParamTargetStore;
class EmitterTable;
class ParamSystem;
class ScheduledTransactionPool;
class TransportEpochOwner;
class HeldCurrentSliceToken;
struct ConsumedCurrentSlice;
class OriginalSourceSubmissionRegistry;
class ImmediateActionLane;
class PrivatePlainLayoutCommitProof;
class PrivateImmediateLanePlacementProof;
class PrivateImmediateLaneGroupsPublicationProof;

using TransportEpoch = std::uint64_t;

inline constexpr TransportEpoch kNoTransportEpoch = 0;
inline constexpr std::uint64_t kVariableCapabilityMaskUnit1 = 0;
inline constexpr std::uint32_t kVariableDefinitionCapacity = 256;
inline constexpr std::uint32_t kVariableHistoryCapacity = 4096;
inline constexpr std::uint32_t kPendingVariableWriteCapacity = 256;
inline constexpr std::uint32_t kVariableWritesPerService = 32;
inline constexpr std::uint32_t kVariableBatchWriteCapacity =
    VN_VARIABLE_BATCH_WRITE_CAPACITY;
inline constexpr std::uint32_t kEmitterBankCapacity = 128;
inline constexpr std::uint32_t kOverridesPerEmitterCapacity = 4096;
inline constexpr std::uint64_t kMaxEmitterVariableBankBytes = 64ull << 20;
inline constexpr std::uint64_t kEmitterBankBudgetBytes = 40;
inline constexpr std::uint64_t kEmitterOverrideBudgetBytes = 56;
inline constexpr float kStepExactIntegerLimit = 16777216.0f;

enum class PackVariablePublicationReason : std::uint8_t {
    ContentMutation,
    InitPublication,
    InitUnload,
};

enum class VariableWriterIdentityV1 : std::uint8_t {
    Game = 1,
    Action = 2,
};

struct VariablePolicySidecarV1 {
    std::uint8_t writers = 0;
    std::uint64_t executionFingerprint = 0;
    std::uint64_t catalogFingerprint = 0;
    const VariableLabelDefinitionV1* labels = nullptr;
    std::uint32_t labelCount = 0;
};

struct VariableLabelCatalogV1 {
    VnVariableId variableId{};
    std::uint64_t fingerprint = 0;
    std::vector<VariableLabelDefinitionV1> labels;
};

enum class VariableKindV1 : std::uint8_t {
    Step = 0,
    Smooth = 1,
};

enum class VariableScopeV1 : std::uint8_t {
    Global = 0,
    Emitter = 1,
};

struct StaticImpactSummary {
    std::uint32_t propertiesPerConsumer = 0;
    std::uint32_t discreteSideReferences = 0;
    std::uint32_t continuousSideReferences = 0;
    std::uint32_t transactionsPerConsumer = 0;
    std::uint32_t eventsPerConsumer = 0;
    std::uint32_t participantsPerConsumer = 0;
    std::uint32_t targetRowsPerConsumer = 0;
    std::uint32_t referencedFlags = 0;
};

struct VariableDefinitionV1 {
    VnVariableId variableId{};
    VariableKindV1 kind = VariableKindV1::Smooth;
    float rangeMin = 0.0f;
    float rangeMax = 0.0f;
    float defaultValue = 0.0f;
    StaticImpactSummary impact{};
};

struct VariableScopeKey {
    VariableScopeV1 scope = VariableScopeV1::Global;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
};

struct VariableCallObservation {
    WorldGeneration world = 0;
    TransportEpoch transport = 0;
    std::uint64_t windowStartFrame = 0;
    std::uint64_t publicationFrontierFrame = 0;
    std::uint64_t requestedEffectiveFrame = 0;
    std::uint32_t quantumFrames = 0;
};

// The one observation formula both the live entry points and the trace
// replayer evaluate: the requested effective frame is the publication
// floor (window start vs publish cutoff, whichever is later) aligned up
// to the next quantum boundary, saturating instead of wrapping.
inline VariableCallObservation ObserveVariableCallAt(
    WorldGeneration world, TransportEpoch transport,
    std::uint32_t quantumFrames, std::uint64_t windowStartFrame,
    std::uint64_t publishCutoffFrame) noexcept {
    VariableCallObservation observation{};
    observation.world = world;
    observation.transport = transport;
    observation.quantumFrames = quantumFrames;
    observation.windowStartFrame = windowStartFrame;
    observation.publicationFrontierFrame = publishCutoffFrame;
    const std::uint64_t floor =
        windowStartFrame > publishCutoffFrame ? windowStartFrame
                                              : publishCutoffFrame;
    const std::uint64_t quantum = quantumFrames;
    observation.requestedEffectiveFrame =
        quantum == 0 || floor > UINT64_MAX - (quantum - 1u)
            ? floor
            : ((floor + quantum - 1u) / quantum) * quantum;
    return observation;
}

enum class VariableOutcomeKind : std::uint32_t {
    Applied = 1,
    Superseded = 2,
    TransportReset = 3,
    EmitterDestroyed = 4,
    WorldReplacement = 5,
    Session = 6,
    TargetRejected = 7,
};

struct VariableOutcomeRecord {
    VariableOutcomeKind outcome = VariableOutcomeKind::Session;
    LogicCommitTicket acceptedTicket{};
    LogicCommitTicket relatedTicket{};
    std::uint64_t actualFrame = 0;
    std::uint64_t bankVersionStableOrdinal = 0;
    WorldGeneration world = 0;
    TransportEpoch transport = 0;
    VnVariableId variableId{};
    VariableScopeV1 scope = VariableScopeV1::Global;
    std::uint32_t emitterGeneration = 0;
};

struct VariableAcceptResult {
    VnResult result = VN_INVALID_ARG;
    VariableCallObservation observation{};
    LogicCommitTicket ticket{};
};

class VariableServiceV1;

namespace test {
void SetSdkCatalogFingerprint(VariableServiceV1& service,
                              std::uint64_t fingerprint) noexcept;
bool SetDefinitionExecutionFingerprint(VariableServiceV1& service,
                                       VnVariableId id,
                                       std::uint64_t fingerprint) noexcept;
}

class PreparedVariableWriteToken {
public:
    PreparedVariableWriteToken() = default;
    PreparedVariableWriteToken(const PreparedVariableWriteToken&) = delete;
    PreparedVariableWriteToken& operator=(
        const PreparedVariableWriteToken&) = delete;
    PreparedVariableWriteToken(PreparedVariableWriteToken&& other) noexcept
        : owner_(other.owner_), slot_(other.slot_),
          prepared_(other.prepared_) {
        other.owner_ = nullptr;
        other.slot_ = UINT32_MAX;
        other.prepared_ = false;
    }
    PreparedVariableWriteToken& operator=(
        PreparedVariableWriteToken&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            slot_ = other.slot_;
            prepared_ = other.prepared_;
            other.owner_ = nullptr;
            other.slot_ = UINT32_MAX;
            other.prepared_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }

private:
    friend class VariableServiceV1;
    friend class PreparedVariableBatchToken;
    VariableServiceV1* owner_ = nullptr;
    std::uint32_t slot_ = UINT32_MAX;
    bool prepared_ = false;
};

class PreparedVariableBatchToken {
public:
    PreparedVariableBatchToken() = default;
    PreparedVariableBatchToken(const PreparedVariableBatchToken&) = delete;
    PreparedVariableBatchToken& operator=(
        const PreparedVariableBatchToken&) = delete;
    PreparedVariableBatchToken(PreparedVariableBatchToken&& other) noexcept;
    PreparedVariableBatchToken& operator=(
        PreparedVariableBatchToken&& other) noexcept;
    ~PreparedVariableBatchToken();

    bool IsPrepared() const noexcept {
        return owner_ != nullptr && count_ != 0 && !committed_;
    }

private:
    friend class VariableServiceV1;
    VariableServiceV1* owner_ = nullptr;
    PreparedVariableWriteToken rows_[kVariableBatchWriteCapacity];
    std::uint32_t count_ = 0;
    bool committed_ = false;
};

class VariableServiceV1 {
public:
    friend void test::SetSdkCatalogFingerprint(VariableServiceV1&,
                                               std::uint64_t) noexcept;
    friend bool test::SetDefinitionExecutionFingerprint(VariableServiceV1&,
                                                        VnVariableId,
                                                        std::uint64_t) noexcept;

    class PreparedPackDefinitions;
    class PreparedCurrentWorldImageV1;
    class PreparedWorldProjectionV1;

    VariableServiceV1(std::uint32_t maxEmitters,
                      std::uint32_t overridesPerEmitter,
                      std::uint32_t maxPropertiesPerCurrentSlice,
                      std::uint32_t maxActionDefinitions,
                      LogicCommitCoordinator& commits,
                      TransportEpochOwner& transport,
                      VariableSliceTable& slices,
                      ScheduledTransactionPool& timeline,
                      std::uint64_t readCutCapacity,
                      std::uint32_t timelineReadUseCapacity,
                      std::uint32_t meterProbeCapacity = 0);

    WorldInstallResult Install(const ContainerWorldInstallViewV1& install);
    WorldInstallResult InstallWithReplacementAdmission(
        const ContainerWorldInstallViewV1& install,
        bool replacementAdmitted);
    WorldInstallResult InstallReplayV2(
        const HistoricalContainerWorldViewV1& install);
    WorldInstallResult InstallReplayV3(
        const HistoricalContainerWorldViewV1& install);
    WorldInstallResult InstallReplayV4(
        const HistoricalContainerWorldViewV1& install);
    WorldGeneration CurrentWorldGeneration() const noexcept {
        return worldGeneration_;
    }
    TransportEpoch CurrentTransportEpoch() const noexcept;
    const WorldIdentityV1* CurrentWorldIdentity() const noexcept {
        return hasWorld_ ? &identity_ : nullptr;
    }
    const VariableDefinitionV1* Definition(VnVariableId id) const noexcept;
    std::uint32_t DefinitionCount() const noexcept {
        return static_cast<std::uint32_t>(definitions_.size());
    }
    const VariableDefinitionV1& DefinitionAt(std::uint32_t index) const {
        return definitions_[index].definition;
    }
    std::uint32_t PlayablePropertyProgramCount() const noexcept {
        return static_cast<std::uint32_t>(playableProperties_.size());
    }
    std::uint32_t PlayableOccurrencePropertyCount() const noexcept {
        return static_cast<std::uint32_t>(playableOccurrences_.size());
    }
    std::uint32_t ContainerProgramCount() const noexcept {
        return static_cast<std::uint32_t>(containerPrograms_.size());
    }
    std::uint32_t TrackScheduleProgramCount() const noexcept {
        return static_cast<std::uint32_t>(trackSchedules_.size());
    }
    std::uint32_t PieceScheduleProgramCount() const noexcept {
        return static_cast<std::uint32_t>(pieceSchedules_.size());
    }
    std::uint32_t TransitionProgramCount() const noexcept {
        return static_cast<std::uint32_t>(transitionPrograms_.size());
    }
    std::uint32_t ActionProgramCount() const noexcept {
        return static_cast<std::uint32_t>(actionPrograms_.size());
    }
    const PlayablePropertyProgramViewV1& PlayablePropertyProgramAt(
        std::uint32_t index) const {
        return playableProperties_[index].view;
    }
    const PlayableOccurrencePropertyViewV1& PlayableOccurrencePropertyAt(
        std::uint32_t index) const {
        return playableOccurrences_[index].view;
    }
    const ContainerProgramViewV3& ContainerProgramAt(
        std::uint32_t index) const {
        return containerPrograms_[index].view;
    }
    const TransitionProgramViewV1& TransitionProgramAt(
        std::uint32_t index) const {
        return transitionPrograms_[index].view;
    }
    const ActionProgramViewV1& ActionProgramAt(std::uint32_t index) const {
        return actionPrograms_[index].view;
    }
    ContainerWorldInstallViewV1 CurrentWorldInstallView() const noexcept {
        ContainerWorldInstallViewV1 view{};
        if (!hasWorld_) return view;
        view.identity = identity_;
        view.variables = definitionViews_.empty()
            ? nullptr : definitionViews_.data();
        view.variableCount = static_cast<std::uint32_t>(definitionViews_.size());
        view.playableProperties = playablePropertyViews_.empty()
            ? nullptr : playablePropertyViews_.data();
        view.playablePropertyCount =
            static_cast<std::uint32_t>(playablePropertyViews_.size());
        view.playableOccurrences = playableOccurrenceViews_.empty()
            ? nullptr : playableOccurrenceViews_.data();
        view.playableOccurrenceCount =
            static_cast<std::uint32_t>(playableOccurrenceViews_.size());
        view.trackSchedules = trackScheduleViews_.empty()
            ? nullptr : trackScheduleViews_.data();
        view.trackScheduleCount =
            static_cast<std::uint32_t>(trackScheduleViews_.size());
        view.pieceSchedules = pieceScheduleViews_.empty()
            ? nullptr : pieceScheduleViews_.data();
        view.pieceScheduleCount =
            static_cast<std::uint32_t>(pieceScheduleViews_.size());
        view.containers = containerProgramViews_.empty()
            ? nullptr : containerProgramViews_.data();
        view.containerCount =
            static_cast<std::uint32_t>(containerProgramViews_.size());
        view.transitions = transitionProgramViews_.empty()
            ? nullptr : transitionProgramViews_.data();
        view.transitionCount =
            static_cast<std::uint32_t>(transitionProgramViews_.size());
        view.actions = actionProgramViews_.empty()
            ? nullptr : actionProgramViews_.data();
        view.actionCount =
            static_cast<std::uint32_t>(actionProgramViews_.size());
        view.capacity = capacity_;
        return view;
    }
    PlayableWorldViewV1 CurrentPlayableWorld() const noexcept {
        PlayableWorldViewV1 view{};
        if (!hasWorld_) return view;
        view.generation = worldGeneration_;
        view.identity = identity_;
        view.properties = playablePropertyViews_.empty()
                              ? nullptr
                              : playablePropertyViews_.data();
        view.propertyCount =
            static_cast<std::uint32_t>(playablePropertyViews_.size());
        view.occurrences = playableOccurrenceViews_.empty()
                               ? nullptr
                               : playableOccurrenceViews_.data();
        view.occurrenceCount =
            static_cast<std::uint32_t>(playableOccurrenceViews_.size());
        view.trackSchedules = trackScheduleViews_.empty()
                                  ? nullptr
                                  : trackScheduleViews_.data();
        view.trackScheduleCount =
            static_cast<std::uint32_t>(trackScheduleViews_.size());
        view.pieceSchedules = pieceScheduleViews_.empty()
                                  ? nullptr
                                  : pieceScheduleViews_.data();
        view.pieceScheduleCount =
            static_cast<std::uint32_t>(pieceScheduleViews_.size());
        view.maxKnownScheduleLeaves = capacity_.maxKnownScheduleLeaves;
        view.maxTrackPieceScheduleDepth =
            capacity_.maxTrackPieceScheduleDepth;
        view.containers = containerProgramViews_.empty()
                              ? nullptr
                              : containerProgramViews_.data();
        view.containerCount =
            static_cast<std::uint32_t>(containerProgramViews_.size());
        view.transitions = transitionProgramViews_.empty()
                               ? nullptr
                               : transitionProgramViews_.data();
        view.transitionCount =
            static_cast<std::uint32_t>(transitionProgramViews_.size());
        return view;
    }

    VariableAcceptResult AcceptGlobal(VnVariableId id, float value,
                                      const VariableCallObservation& observed,
                                      ParamSystem& params,
                                      BindingTable& bindings,
                                      BusTargetStore& busTargets);
    VariableAcceptResult AcceptEmitter(VnEmitterHandle emitter, VnVariableId id,
                                       float value,
                                       const VariableCallObservation& observed,
                                       const EmitterTable& emitters,
                                       ParamSystem& params);
    VariableAcceptResult AcceptGlobalBatch(
        const VnVariableWriteV1* writes, std::uint32_t writeCount,
        const VariableCallObservation& observed, ParamSystem& params,
        BindingTable& bindings, BusTargetStore& busTargets);
    VariableAcceptResult AcceptEmitterBatch(
        VnEmitterHandle emitter, const VnVariableWriteV1* writes,
        std::uint32_t writeCount, const VariableCallObservation& observed,
        const EmitterTable& emitters, ParamSystem& params);
    VariableAcceptResult AcceptGlobalResetBatch(
        const VnVariableWriteV1* writes, std::uint32_t writeCount,
        const VariableCallObservation& observed, ParamSystem& params,
        BindingTable& bindings, BusTargetStore& busTargets);
    VariableAcceptResult PrepareGlobalWrite(
        VnVariableId id, float value,
        const VariableCallObservation& observed,
        std::uint32_t executionOrdinal,
        PreparedVariableWriteToken* out,
        VariableWriterIdentityV1 writer = VariableWriterIdentityV1::Game) noexcept;
    VariableAcceptResult PrepareEmitterWrite(
        VnEmitterHandle emitter, VnVariableId id, float value,
        const VariableCallObservation& observed,
        std::uint32_t executionOrdinal,
        const EmitterTable& emitters,
        PreparedVariableWriteToken* out,
        VariableWriterIdentityV1 writer = VariableWriterIdentityV1::Game) noexcept;
    void CommitPreparedWrite(PreparedVariableWriteToken* prepared,
                             LogicCommitSequence sequence) noexcept;
    bool BindPreparedActionWriteOrder(PreparedVariableWriteToken& prepared,
        const LogicCommitCoordinator::PreparedIssue& issue) noexcept;
    bool BindPreparedActionWriteOrder(PreparedVariableWriteToken& prepared, Engine& engine,
        const ActionFiringTable::PreparedChildIdentity& child) noexcept;
    void CommitPreparedActionWrite(PreparedVariableWriteToken* prepared,
        const LogicCommitCoordinator::ConsumedIssue& consumed) noexcept;
    void CommitPreparedActionWrite(PreparedVariableWriteToken* prepared,
        const ActionFiringTable::ChildActivationReceipt& activated) noexcept;
    void CommitPreparedActionSources(const LogicCommitCoordinator::ConsumedIssue& consumed) noexcept;
    void CommitPreparedActionSources(const ActionFiringTable::ChildActivationReceipt& activated) noexcept;
    bool SealPreparedActionSources(const LogicCommitCoordinator::PreparedIssue& issue) noexcept;
    bool SealPreparedActionSources(Engine& engine,
        const ActionFiringTable::PreparedChildIdentity& child) noexcept;
    void CancelPreparedWrite(PreparedVariableWriteToken* prepared) noexcept;

    void BeginService(Engine& engine, std::uint64_t blocksRendered,
                      std::uint32_t blockFrames);
    void EndService(Engine& engine);
    void PrepareClaimedSlices(Engine& engine);
    void PublishPreparedPlaybackPages(std::uint64_t requestedPublishThrough = 0) noexcept;
    bool PrepareReadCutSources(Engine& engine, VariableReadCutRef cut);
    bool PrepareActionReadCutSources(Engine& engine, VariableReadCutRef cut,
        const LogicCommitCoordinator::PreparedIssue& issue);
    bool PrepareActionReadCutSources(Engine& engine, VariableReadCutRef cut,
        const ActionFiringTable::PreparedChildIdentity& child);
    std::uint64_t EarliestReadCutFrame(const Engine& engine) const noexcept;
    void TerminateSession();
    bool NoteEmitterCreated(VnEmitterHandle emitter) noexcept;
    void NoteEmitterDestroyed(Engine& engine, VnEmitterHandle emitter);

    bool ResetTransport(std::uint64_t resetFrame);
    bool HasOutstandingWork() const noexcept;
    bool DrainingWorld() const noexcept {
        return drainState_ == WorldDrainState::DrainingCurrent;
    }
    bool DrainingTransport() const noexcept {
        return transportDrainState_ != TransportDrainState::Idle;
    }
    bool HasUnreclaimedLiveWorldRows() const noexcept {
        return slices_.HasUnreclaimedWorldRows(worldGeneration_);
    }
    bool SemanticIdenticalWorld(
        const ContainerWorldInstallViewV1& install) const {
        return SemanticIdentical(install);
    }
    bool AdmissionClosed() const noexcept {
        return engineAdmissionClosed_ || DrainingWorld() ||
               DrainingTransport();
    }
    void SetEngineAdmissionClosed(bool closed) noexcept {
        engineAdmissionClosed_ = closed;
    }
    bool HasBlockingWrite(VnVariableId id) const noexcept;
    bool BlocksConsumerAdmission(VnVariableId id) const noexcept {
        return AdmissionClosed() || HasBlockingWrite(id);
    }
    bool BlocksVariableConsumer(VnVariableId id) const noexcept {
        return BlocksConsumerAdmission(id);
    }
    void NoteObservedBlock(std::uint64_t blocksRendered) noexcept {
        observedBlock_ = blocksRendered;
    }

    bool CurrentValue(VnVariableId id, float* out) const noexcept;
    bool CurrentValue(VnEmitterHandle emitter, VnVariableId id,
                      float* out) const noexcept;
    bool CaptureReadCut(VnEmitterHandle emitter, std::uint64_t executeFrame,
                        LogicCommitSequence sequence,
                        std::uint32_t executionOrdinalExclusive,
                        VariableReadCutRef* out) noexcept;
    bool CaptureActionReadCut(VnEmitterHandle emitter, std::uint64_t executeFrame,
        const LogicCommitCoordinator::PreparedIssue& issue,
        std::uint32_t executionOrdinalExclusive, VariableReadCutRef* out,
        bool pendingPlacement = false) noexcept;
    bool CaptureActionReadCut(Engine& engine, VnEmitterHandle emitter, std::uint64_t executeFrame,
        const ActionFiringTable::PreparedChildIdentity& child,
        std::uint32_t executionOrdinalExclusive, VariableReadCutRef* out,
        bool pendingPlacement = false) noexcept;
    bool ReadCutValue(VariableReadCutRef cut, VnVariableId id,
                      float* out) noexcept;
    VariableReadCutValueStatus ReadCutInput(VariableReadCutRef cut, VnVariableId id,
                                           float* out) noexcept;
    VariableReadCutReader ReadCutReader(VariableReadCutRef cut) noexcept;
    bool ReadCutHasSources(VariableReadCutRef cut) const noexcept;
    VariableReadCutStatus PublishReadCut(VariableReadCutRef cut,
                                        std::uint64_t finalFrame) noexcept;
    // Source proof only; the start consumer must also validate its published identities.
    VariableReadCutStatus ReadCutSourcesStatus(VariableReadCutRef cut) const noexcept;
    bool ReserveNativeReadCutCalendar(VariableReadCutRef cut, std::uint64_t publicationFrontier,
                                      HeldCurrentSliceToken* out) noexcept;
    bool CancelNativeReadCutCalendar(VariableReadCutRef cut, HeldCurrentSliceToken* held) noexcept;
    bool HasNativeReadCutsForTransport(TransportEpoch transport) const noexcept;
    bool ConsumeNativeReadCutCalendar(const ConsumedCurrentSlice& consumed) noexcept;
    bool NativeReadCutCalendarConsumed(const VariableNativeStartIdentity& identity) const noexcept;
    bool CaptureNativeReadCutIdentity(VariableReadCutRef cut, VnVoiceHandle nativeHandle,
                                      VariableNativeStartIdentity* out) const noexcept;
    bool PrepareNativeReadCut(VariableReadCutRef cut, VnVoiceHandle nativeHandle,
                              std::uint64_t finalFrame, VariableNativeStartIdentity* out) noexcept;
    bool CommitNativeReadCut(const VariableNativeStartIdentity& identity) noexcept;
    bool CancelPreparedNativeReadCut(const VariableNativeStartIdentity& identity) noexcept;
    // Audio marks the exact native start only after its complete initial command run is installed.
    bool MarkNativeReadCutInstalled(const VariableNativeStartIdentity& identity) noexcept;
    VariableNativeStartReceipt NativeReadCutStatus(const VariableNativeStartIdentity& identity) const noexcept;
    VariableReadCutStatus NativeReadCutSourcesStatus(const VariableNativeStartIdentity& identity) const noexcept;
    // Audio detaches every native link, then publishes activation/failure. No cut reads follow this call.
    bool FinishNativeReadCut(const VariableNativeStartIdentity& identity, bool activated) noexcept;
    // Logic acquires the terminal receipt before releasing any source pins.
    bool AcknowledgeNativeReadCut(const VariableNativeStartIdentity& identity) noexcept;
    void AcknowledgeFinishedNativeReadCuts() noexcept;
    bool HasNativePublicationHold(VnVoiceHandle nativeHandle) const noexcept;
    void ShutdownReadCutsAfterQuiescence() noexcept;
    bool CanReleaseReadCut(VariableReadCutRef cut) const noexcept;
    bool ReleaseReadCut(VariableReadCutRef cut) noexcept;
    bool PrepareTimelineReadUse(VariableReadCutRef cut, VnVoiceHandle nativeHandle,
        TimelineArmTable& arms, const TimelineArmTable::PreparedCommitProof& proof,
        VariableTimelineReadUseRef* out, std::uint64_t authoredStartOffset = 0) noexcept;
    bool BindPreparedTimelineReadUseSource(VariableTimelineReadUseRef use,
        OriginalSourceSubmissionRegistry& sources, OriginalSourceLifetimePin&& original) noexcept;
    bool BeginPendingTimelineReadUsePlacement(VariableTimelineReadUseRef use,
        const ImmediateActionLane& lane, const PrivatePlainLayoutCommitProof& placement,
        VariableReadCutRef* cut) noexcept;
    bool BeginPendingTimelineReadUsePlacement(VariableTimelineReadUseRef use,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        VariableReadCutRef* cut) noexcept;
    bool BeginPendingVariableGroupPlacement(Engine& engine, EngineActionPlayScratch& producer,
        EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        VariableReadCutReader* out) noexcept;
    bool BeginPendingVariableGroupReprepare(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, std::uint64_t anchor,
        VariableReadCutReader* out) noexcept;
    bool PreparePendingOrdinarySelectionReadCut(Engine& engine, EngineActionPlayScratch& producer,
        EngineImmediateEntryAttempt& attempt, std::uint64_t anchor) noexcept;
    bool OriginalPendingVariableGroupUsesMatch(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) const noexcept;
    bool StageOriginalPendingVariableGroupInitializer(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t leaf, std::span<const CommandRecord> commands) noexcept;
    bool OriginalPendingVariableGroupReadyToFreeze(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        bool* outWaiting) noexcept;
    bool FreezeOriginalPendingVariableGroup(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    bool MatchesPreparedTimelineReadUse(VariableTimelineReadUseRef use, VariableReadCutRef cut,
        VnVoiceHandle nativeHandle, const TimelineArmTable::PreparedCommitProof& arm,
        const OriginalSourceSubmissionRegistry& sources, std::uint64_t authoredOffset) const noexcept;
    bool MatchesPrivateCommittedTimelineReadUse(VariableTimelineReadUseRef use, VariableReadCutRef cut,
        VnVoiceHandle nativeHandle, const TimelineArmTable& arms, TimelineArmToken arm,
        const OriginalSourceSubmissionRegistry& sources, std::uint64_t anchor, std::uint64_t authoredOffset) const noexcept;
    bool MatchesPublishedPendingVariableGroupReadCut(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const EngineActionPlayScratch& child,
        const PrivateImmediateLanePlacementProof& placement, bool entryBound) const noexcept;
    bool MatchesEntryBoundTimelineReadUse(VariableTimelineReadUseRef use, VariableReadCutRef cut,
        VnVoiceHandle nativeHandle, const TimelineArmTable& arms, TimelineArmToken arm,
        const OriginalSourceSubmissionRegistry& sources, const DecisionTable& decisions,
        const ScheduledTransactionPool& pool, const FrontierActivationDescriptor& descriptor,
        std::uint32_t leafCount, std::uint64_t authoredOffset) const noexcept;
    bool BeginPreparedPendingVariableGroupPlacement(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        VariableReadCutReader* out) noexcept;
    bool StagePreparedPendingVariableGroupInitializer(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t leaf, std::span<const CommandRecord> commands) noexcept;
    bool FreezePreparedPendingVariableGroup(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        bool* outWaiting = nullptr) noexcept;
    bool PreparedPendingVariableGroupReadyToFreeze(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        bool* outWaiting = nullptr) noexcept;
    bool ResetPrivatePendingVariableGroupCut(Engine& engine, EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt) noexcept;
    bool FreezePendingTimelineReadUse(VariableTimelineReadUseRef use,
        std::span<const CommandRecord> commands) noexcept;
    bool ProveTimelineReadUseInitializer(VariableTimelineReadUseRef use, VnVoiceHandle nativeHandle,
        const TimelineArmTable::PlainAudioProof& proof, VariableTimelineInitializerProof* out) noexcept;
    bool ProveTimelineReadUseInitializer(VariableTimelineReadUseRef use, VnVoiceHandle nativeHandle,
        const TimelineArmTable::LaneAudioProof& proof, VariableTimelineInitializerProof* out) noexcept;
    bool ProveTimelineReadUseInitializer(VariableTimelineReadUseRef use, VnVoiceHandle nativeHandle,
        const TimelineArmTable::BoundAudioProof& proof, VariableTimelineInitializerProof* out) noexcept;
    bool ProveTimelineReadUseInitializer(VariableTimelineReadUseRef use, VnVoiceHandle nativeHandle,
        const TimelineArmTable::AudioOperationClaim& proof, VariableTimelineInitializerProof* out) noexcept;
    static void ApplyTimelineReadUseInitializer(VariableTimelineInitializerProof& proof,
        void* context, void (*apply)(void*, const CommandRecord&) noexcept) noexcept;
    bool PendingTimelineReadUseFrozen(VariableTimelineReadUseRef use) const noexcept;
    bool PendingTimelineReadUseStaged(VariableTimelineReadUseRef use) const noexcept;
    bool CommitTimelineReadUse(VariableTimelineReadUseRef use) noexcept;
    bool CancelPreparedTimelineReadUse(VariableTimelineReadUseRef use) noexcept;
    bool CanTransferReadCutToTimelineUses(VariableReadCutRef cut) const noexcept;
    bool TransferReadCutToTimelineUses(VariableReadCutRef cut) noexcept;
    bool PrepareFrontierEntryReadUse(VariableReadCutRef cut, DecisionTable& decisions,
        ScheduledTransactionPool& pool, const FrontierActivationDescriptor& descriptor) noexcept;
    bool CommitFrontierEntryReadUse(VariableReadCutRef cut) noexcept;
    bool CanCommitFrontierEntryReadUse(VariableReadCutRef cut,
        const DecisionTable::FrontierPublicationProof& activation,
        const PrivateImmediateLaneGroupsPublicationProof& groups) const noexcept;
    bool CommitFrontierEntryReadUse(VariableReadCutRef cut,
        const DecisionTable::FrontierPublicationProof& activation,
        const PrivateImmediateLaneGroupsPublicationProof& groups) noexcept;
    VariableReadCutStatus FrontierEntrySourcesStatus(
        const FrontierActivationDescriptor& descriptor) const noexcept;
    bool FinishFrontierEntryReadUse(const FrontierEntryReceiptRef& entry,
        std::uint64_t completedAudioSequence) noexcept;
    bool CancelPreparedFrontierEntryReadUse(const FrontierEntryReceiptRef& entry) noexcept;
    VariableReadCutStatus TimelineReadUseSourcesStatus(VariableTimelineReadUseRef use,
        VnVoiceHandle nativeHandle) const noexcept;
    VariableReadCutStatus TimelineReadUseStartStatus(VariableTimelineReadUseRef use,
        VnVoiceHandle nativeHandle) const noexcept;
    bool PendingTimelineReadUseMatchesArm(VariableTimelineReadUseRef use,
        VnVoiceHandle nativeHandle, TimelineArmToken arm) const noexcept;
    void AcknowledgeFinishedTimelineReadUses(std::uint64_t completedAudioSequence) noexcept;
    std::uint32_t ReadCutCapacity() const noexcept { return readCutCapacity_; }
    std::uint32_t TimelineReadUseCapacity() const noexcept { return timelineReadUseCapacity_; }
    std::size_t ReadCutStorageBytes() const noexcept;
    std::size_t StorageBytes() const noexcept;
    std::uint32_t RevisionCapacity() const noexcept { return revisionCapacity_; }
    std::uint32_t RevisionStorageCapacity() const noexcept { return static_cast<std::uint32_t>(history_.size()); }
    std::uint32_t CompletionCapacity() const noexcept { return completionCapacity_; }
    std::uint32_t ApplyPayloadCapacity() const noexcept { return applyPayloadCapacity_; }
    bool HasStagedOutcomes() const noexcept { return !outcomes_.empty(); }
    bool CaptureCurrentBank(VnEmitterHandle emitter,
                            const VnVariableId* variableIds,
                            std::uint32_t variableCount,
                            float* outValues,
                            BankVersionToken* outToken) const noexcept;
    bool CaptureCurrentBankWithPreparedOverlay(
        VnEmitterHandle emitter, const VnVariableId* variableIds,
        std::uint32_t variableCount,
        std::uint32_t executionOrdinalExclusive, float* outValues,
        BankVersionToken* outToken) const noexcept;
    bool CaptureCurrentBankWithPreparedOverlay(
        VnEmitterHandle emitter, const VnVariableId* variableIds,
        std::uint32_t variableCount, std::uint64_t executeFrame,
        LogicCommitSequence sequence,
        std::uint32_t executionOrdinalExclusive, float* outValues,
        BankVersionToken* outToken) const noexcept;
    bool HasPendingBankWrite(VnEmitterHandle emitter,
                             const VnVariableId* variableIds,
                             std::uint32_t variableCount) const noexcept;
    bool HasPendingWriteForVoiceTarget(const ParamSystem& params,
                                       VnVoiceHandle handle,
                                       std::uint32_t targetId) const noexcept;
    bool HasPendingWriteForBusTarget(const BindingTable& bindings,
                                     std::uint32_t busIndex,
                                     std::uint32_t targetId) const noexcept;
    bool ScheduledValue(VnEmitterHandle emitter, VnVariableId id,
                        std::uint64_t frame, float* out,
                        std::uint64_t* bankOrdinal = nullptr,
                        std::uint64_t* requestedFrame = nullptr) const noexcept;

    bool PopOutcome(VariableOutcomeRecord* out) noexcept;
    void MarkOutcomeDeliveryComplete() noexcept;
    void FinishOutcomeDelivery() noexcept;
    void QueryVariableTicket(const VnVariableTicketV1& ticket,
                             std::uint32_t* status,
                             std::uint32_t* outcome) const noexcept;
    VnResult QueryVariable(VnVariableId id, VnEmitterHandle emitter,
                           const EmitterTable& emitters,
                           VnVariableQueryResultV1* out) const noexcept;

    // Projects the current world image (Global defaults plus applied values
    // and live emitter overrides) into the evaluation stores, so bindings
    // resolve installed definitions immediately after a world install.
    void ProjectCurrentWorld(ParamSystem& params, EmitterTable& emitters,
                             BindingTable& bindings,
                             BusTargetStore& busTargets) noexcept;

    // Replaces the installed Variable definition table from a Pack composite.
    // Does not install Action or playable programs, close admission, or drain.
    VnResult ReplacePackDefinitions(
        const VariableDefinitionV1* definitions, std::uint32_t count);
    VnResult PreparePackDefinitions(
        const VariableDefinitionV1* definitions, std::uint32_t count,
        PreparedPackDefinitions* out) const;
    VnResult PreparePackDefinitions(
        const VariableDefinitionV1* definitions, std::uint32_t count,
        PackVariablePublicationReason reason, std::uint64_t actualFrame,
        PreparedPackDefinitions* out) const;
    VnResult PreparePackDefinitions(
        const VariableDefinitionV1* definitions, std::uint32_t count,
        const VariablePolicySidecarV1* overlays,
        PackVariablePublicationReason reason, std::uint64_t actualFrame,
        PreparedPackDefinitions* out) const;
    void CommitPreparedPackDefinitions(
        PreparedPackDefinitions* prepared) noexcept;
    void CommitPreparedPackDefinitions(
        PreparedPackDefinitions* prepared, ParamSystem& params,
        EmitterTable& emitters, BindingTable& bindings,
        BusTargetStore& busTargets) noexcept;
    WorldInstallStatus PrepareCurrentWorldImageV1(
        const ContainerWorldInstallViewV1& install,
        PreparedCurrentWorldImageV1& out);
    WorldGeneration CommitPreparedCurrentWorldImageV1(
        PreparedCurrentWorldImageV1& prepared) noexcept;
    bool CanCommitPreparedCurrentWorldImageV1(
        const PreparedCurrentWorldImageV1& prepared) const noexcept;
    ContainerWorldInstallViewV1 PreparedWorldInstallViewV1(
        const PreparedCurrentWorldImageV1& prepared) const noexcept;
    bool PrepareWorldProjectionV1(const PreparedCurrentWorldImageV1& image,
        const EmitterTable& emitters, PreparedWorldProjectionV1* out) const;
    bool CanCommitWorldProjectionV1(const PreparedWorldProjectionV1& prepared,
        const EmitterTable& emitters) const noexcept;
    void CommitWorldProjectionV1(PreparedWorldProjectionV1* prepared,
        ParamSystem& params, EmitterTable& emitters,
        BindingTable& bindings, BusTargetStore& busTargets) noexcept;

    std::uint64_t AcceptedCount() const noexcept { return acceptedCount_; }
    std::uint64_t AppliedCount() const noexcept { return appliedCount_; }
    std::uint64_t SupersededCount() const noexcept { return supersededCount_; }
    std::uint64_t PendingCount() const noexcept;
    std::uint64_t TerminationCount() const noexcept { return terminationCount_; }
    std::uint64_t HistoryHighWater() const noexcept { return historyHighWater_; }
    std::uint64_t RefusalCount() const noexcept { return refusalCount_; }
    std::uint32_t HistoryCapacity() const noexcept {
        return queryHistoryCapacity_;
    }
    std::uint32_t PendingCapacity() const noexcept {
        return revisionCapacity_;
    }
    std::uint64_t ExecutionCatalogFingerprint() const noexcept;
    VnResult ResolveLabel(VnVariableId variable, VnObjectId label, float* value) const noexcept;
    VnResult ResolveLabel(VnVariableId variable, std::string_view name, float* value) const noexcept;
    std::uint64_t SdkCatalogFingerprint() const noexcept {
        return sdkCatalogFingerprint_;
    }
    VariableSliceRef SliceForTicketForTests(
        std::uint64_t ticket) const noexcept;
    bool EmitterBankActiveForTests(
        VnEmitterHandle emitter) const noexcept;
    bool CanMintTransportResetBankVersions() const noexcept;

private:
    friend struct detail::VariableServiceTestAccess;

    enum class WriteOperation : std::uint8_t { Set, Reset };
    enum class WorldDrainState : std::uint8_t { Idle, DrainingCurrent };
    enum class TransportDrainState : std::uint8_t {
        Idle,
        DrainingCurrent,
        EpochRaised,
    };
    enum class RowState : std::uint8_t {
        Pending,
        TargetsClaimed,
        WaitingFrontier,
        SlicePrepared,
        PageCommitted,
        Applied,
        Terminal,
    };

    struct DefinitionRow {
        VariableDefinitionV1 definition{};
        std::uint8_t writers = 0;
        std::uint64_t executionFingerprint = 0;
        float currentGlobal = 0.0f;
        float scheduledGlobal = 0.0f;
        std::uint64_t currentBankOrdinal = 0;
        std::uint64_t scheduledBankOrdinal = 0;
    };

    struct OwnedPayload {
        PlayablePropertyPayloadViewV1 view{};
        std::vector<SendV1> sends;
        std::vector<EffectNodeV1> effectNodes;
        std::vector<std::vector<EffectParamV1>> effectParams;
        std::vector<EffectEdgeV1> effectEdges;

        void Refresh() noexcept;
    };

    struct OwnedPropertyProgram {
        PlayablePropertyProgramViewV1 view{};
        std::vector<LayerAncestorRefV1> layerAncestors;
        std::vector<StructuralAncestorRefV1> structuralAncestors;
        std::vector<OwnedPayload> structuralPayloads;
        OwnedPayload payload;

        void Refresh() noexcept;
    };

    struct OwnedOccurrenceProgram {
        PlayableOccurrencePropertyViewV1 view{};
        OwnedPayload payload;

        void Refresh() noexcept;
    };

    struct OwnedContainerProgram {
        ContainerProgramViewV3 view{};
        std::vector<ContainerChildProgramViewV2> children;
        std::vector<CurveV1> curves;
        std::vector<std::vector<AuthoredCompletionLeafV1>> completionLeaves;
        std::vector<std::vector<std::vector<StableAuthoredPathStepV1>>>
            completionPaths;
        std::vector<GridAxisV1> discreteAxes;
        std::vector<DiscreteGridRowViewV1> discreteRows;
        std::vector<std::vector<DiscreteGridCellV1>> discreteCells;
        std::vector<GridAxisV1> continuousAxes;
        std::vector<ContinuousGridRowViewV1> continuousRows;
        std::vector<std::vector<ContinuousGridCellV1>> continuousCells;

        void Refresh() noexcept;
    };

    struct OwnedTrackScheduleProgram {
        TrackScheduleProgramViewV1 view{};
        std::vector<ScheduleClipPlacementV1> placements;

        void Refresh() noexcept;
    };

    struct OwnedPieceScheduleProgram {
        PieceScheduleProgramViewV1 view{};
        std::vector<ScheduleTrackRefV1> trackRefs;
        std::vector<ScheduleMarkerV1> markers;

        void Refresh() noexcept;
    };

    struct OwnedTransitionProgram {
        TransitionProgramViewV1 view{};
        std::vector<StableAuthoredPathStepV1> downwardPathSteps;
        std::vector<TransitionRuleViewV1> rules;

        void Refresh() noexcept;
    };

    struct OwnedActionProgram {
        ActionProgramViewV1 view{};
        std::vector<ActionStepProgramViewV1> steps;

        void Refresh() noexcept;
    };

    struct OverrideRow {
        bool used = false;
        VnVariableId variableId{};
        float currentValue = 0.0f;
        float scheduledValue = 0.0f;
        std::uint64_t currentBankOrdinal = 0;
        std::uint64_t scheduledBankOrdinal = 0;
        std::uint32_t pendingSetCount = 0;
    };

    struct EmitterBank {
        bool live = false;
        std::uint32_t generation = 0;
        std::vector<OverrideRow> overrides;
    };

public:
    class PreparedCurrentWorldImageV1 {
    public:
        PreparedCurrentWorldImageV1() = default;
        PreparedCurrentWorldImageV1(const PreparedCurrentWorldImageV1&) = delete;
        PreparedCurrentWorldImageV1& operator=(
            const PreparedCurrentWorldImageV1&) = delete;
        PreparedCurrentWorldImageV1(
            PreparedCurrentWorldImageV1&&) noexcept = default;
        PreparedCurrentWorldImageV1& operator=(
            PreparedCurrentWorldImageV1&&) noexcept = default;

        bool Valid() const noexcept { return valid_; }
        bool Idempotent() const noexcept { return idempotent_; }
        void Clear() noexcept { *this = PreparedCurrentWorldImageV1{}; }

    private:
        friend class VariableServiceV1;
        bool valid_ = false;
        bool idempotent_ = false;
        const VariableServiceV1* source_ = nullptr;
        WorldGeneration sourceWorld_ = 0;
        std::uint64_t sourceBankOrdinal_ = 0;
        WorldIdentityV1 identity_{};
        WorldCapacitySummaryV1 capacity_{};
        std::vector<DefinitionRow> definitions_;
        std::vector<VariableDefinitionV1> definitionViews_;
        std::vector<OwnedPropertyProgram> playableProperties_;
        std::vector<PlayablePropertyProgramViewV1> playablePropertyViews_;
        std::vector<OwnedOccurrenceProgram> playableOccurrences_;
        std::vector<PlayableOccurrencePropertyViewV1> playableOccurrenceViews_;
        std::vector<OwnedTrackScheduleProgram> trackSchedules_;
        std::vector<TrackScheduleProgramViewV1> trackScheduleViews_;
        std::vector<OwnedPieceScheduleProgram> pieceSchedules_;
        std::vector<PieceScheduleProgramViewV1> pieceScheduleViews_;
        std::vector<OwnedContainerProgram> containerPrograms_;
        std::vector<ContainerProgramViewV3> containerProgramViews_;
        std::vector<OwnedTransitionProgram> transitionPrograms_;
        std::vector<TransitionProgramViewV1> transitionProgramViews_;
        std::vector<OwnedActionProgram> actionPrograms_;
        std::vector<ActionProgramViewV1> actionProgramViews_;
    };

    class PreparedWorldProjectionV1 {
    public:
        PreparedWorldProjectionV1() = default;
        PreparedWorldProjectionV1(const PreparedWorldProjectionV1&) = delete;
        PreparedWorldProjectionV1& operator=(const PreparedWorldProjectionV1&) = delete;
        bool IsPrepared() const noexcept { return owner_ != nullptr; }

        template <typename Fn>
        void ForEachGlobal(Fn&& fn) const {
            for (const auto& row : globals_)
                fn(row.id, row.value);
        }

    private:
        friend class VariableServiceV1;
        void Clear() noexcept {
            owner_ = nullptr;
            sourceWorld_ = 0;
            sourceBankOrdinal_ = 0;
            globals_.clear();
            emitters_.clear();
        }
        struct Value { VnVariableId id{}; float value = 0.0f; };
        struct EmitterValues {
            bool bankLive = false;
            std::uint32_t generation = 0;
            VnEmitterHandle handle = 0;
            bool emitterLive = false;
            std::vector<Value> values;
        };
        const VariableServiceV1* owner_ = nullptr;
        WorldGeneration sourceWorld_ = 0;
        std::uint64_t sourceBankOrdinal_ = 0;
        std::vector<Value> globals_;
        std::vector<EmitterValues> emitters_;
    };

    class PreparedPackDefinitions {
    public:
        PreparedPackDefinitions() = default;
        PreparedPackDefinitions(const PreparedPackDefinitions&) = delete;
        PreparedPackDefinitions& operator=(const PreparedPackDefinitions&) =
            delete;
        PreparedPackDefinitions(PreparedPackDefinitions&&) noexcept;
        PreparedPackDefinitions& operator=(
            PreparedPackDefinitions&&) noexcept;

        bool IsPrepared() const noexcept { return owner_ != nullptr; }

        template <typename Fn>
        void ForEachGlobal(Fn&& fn) const {
            for (const auto& row : definitions_)
                fn(row.definition.variableId, row.currentGlobal);
        }

    private:
        friend class VariableServiceV1;
        struct TerminationRow {
            std::uint32_t historySlot = 0;
            LogicCommitTicket ticket{};
        };
        const VariableServiceV1* owner_ = nullptr;
        PackVariablePublicationReason reason_ = PackVariablePublicationReason::ContentMutation;
        std::vector<VariableLabelCatalogV1> labelCatalog_;
        std::uint64_t sdkCatalogFingerprint_ = 0;
        WorldGeneration priorWorld_ = 0;
        TransportEpoch priorTransport_ = 0;
        std::uint64_t actualFrame_ = 0;
        std::vector<TerminationRow> terminations_;
        bool hasWorld_ = false;
        WorldGeneration worldGeneration_ = 0;
        std::vector<DefinitionRow> definitions_;
        std::vector<VariableDefinitionV1> definitionViews_;
        std::vector<EmitterBank> emitterBanks_;
    };

private:

    static_assert(sizeof(EmitterBank) <= kEmitterBankBudgetBytes);
    static_assert(sizeof(OverrideRow) == kEmitterOverrideBudgetBytes);

    struct TicketQueryRecord {
        LogicCommitSequence sequence = 0;
        WorldGeneration world = 0;
        TransportEpoch transport = 0;
        VariableOutcomeKind outcome = VariableOutcomeKind::Applied;
        bool pending = false;
    };

    struct ActionOrderIdentity {
        LogicCommitCoordinator::PreparedIssueIdentity ordinary{};
        ActionFiringTable::PreparedChildIdentity deferred{};
        const ActionFiringTable* deferredTable = nullptr;
        bool HasReservation() const noexcept {
            return ordinary.HasReservation() || deferred.HasPreparation();
        }
        bool SameReservation(const ActionOrderIdentity& other) const noexcept {
            return ordinary.SameReservation(other.ordinary) ||
                (deferredTable != nullptr && deferredTable == other.deferredTable &&
                    deferred.SamePreparation(other.deferred));
        }
    };
    bool CaptureActionOrder(const LogicCommitCoordinator::PreparedIssue& issue,
        ActionOrderIdentity* out) const noexcept;
    bool CaptureActionOrder(Engine& engine, const ActionFiringTable::PreparedChildIdentity& child,
        ActionOrderIdentity* out) const noexcept;
    bool IsPreparedActionOrderActive(const ActionOrderIdentity& identity) const noexcept;
    bool MatchesConsumedActionOrder(const ActionOrderIdentity& identity,
        const LogicCommitCoordinator::ConsumedIssue* consumed,
        const ActionFiringTable::ChildActivationReceipt* activated) const noexcept;
    bool BindPreparedActionWriteOrderImpl(PreparedVariableWriteToken& prepared,
        const ActionOrderIdentity& identity, LogicCommitSequence sequence) noexcept;
    bool SealPreparedActionSourcesImpl(const ActionOrderIdentity& identity) noexcept;
    void CommitPreparedActionSourcesImpl(const LogicCommitCoordinator::ConsumedIssue* consumed,
        const ActionFiringTable::ChildActivationReceipt* activated) noexcept;
    bool PrepareActionReadCutSourcesImpl(Engine& engine, VariableReadCutRef cut,
        const ActionOrderIdentity& identity);

    struct WriteRow {
        bool used = false;
        bool reserved = false;
        ActionOrderIdentity preparedIssue{};
        Engine* preparedEngine = nullptr;
        bool cancelledBeforePublication = false;
        bool preparedSourcesSealed = false;
        RowState state = RowState::Pending;
        WriteOperation operation = WriteOperation::Set;
        WorldGeneration world = 0;
        TransportEpoch transport = 0;
        VariableScopeKey scope{};
        VnVariableId variableId{};
        float value = 0.0f;
        std::uint64_t requestedFrame = 0;
        LogicCommitTicket ticket{};
        std::uint32_t executionOrdinal = 0;
        std::uint64_t requiredCommandFrontier = 0;
        std::uint64_t committedFrame = 0;
        std::uint64_t bankOrdinal = 0;
        VariableSliceRef slice{};
        TargetClaimOwner publicationOwner{};
        bool frontierCaptured = false;
        bool placementHeld = false;
        bool emitterDestroyedBeforeApplied = false;
        std::uint32_t emitterOverrideSlot = UINT32_MAX;
        std::uint64_t outcomeGeneration = 0;
        std::uint64_t batchId = 0;
        bool batchLeader = true;
        std::uint32_t batchHead = UINT32_MAX;
        std::uint32_t batchNext = UINT32_MAX;
        std::uint32_t observers = 0;
        bool historySlicePin = false;
        std::uint64_t consumedFence = 0;
        bool outcomeDelivered = false;
        VariableOutcomeKind finalOutcome = VariableOutcomeKind::Session;
    };

    struct HistoryRef {
        std::uint32_t slot = UINT32_MAX;
        std::uint32_t generation = 0;
        LogicCommitSequence sequence = 0;
        WorldGeneration world = 0;
        TransportEpoch transport = 0;
        bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
    };
    struct ReadCutValueRow {
        VnVariableId variableId{};
        float value = 0.0f;
        HistoryRef source{};
        std::uint64_t baselineOrdinal = 0;
        bool read = false;
    };
    enum class ReadCutState : std::uint8_t { Free, Capturing, Published, Quarantined };
    enum class EntryReadUseState : std::uint8_t { None, Prepared, Published };
    struct CandidatePins {
        std::span<std::uint64_t> words;
        void reset() noexcept { std::fill(words.begin(), words.end(), 0); }
        void set(std::size_t index) noexcept { words[index / 64] |= std::uint64_t{1} << (index % 64); }
        bool test(std::size_t index) const noexcept {
            return (words[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
        }
        std::size_t count() const noexcept {
            std::size_t result = 0;
            for (auto word : words) result += std::popcount(word);
            return result;
        }
    };
    struct ReadCutRow {
        std::atomic<std::uint32_t> generation{1};
        std::atomic<ReadCutState> state{ReadCutState::Free};
        WorldGeneration world = 0;
        TransportEpoch transport = 0;
        VnEmitterHandle emitter = 0;
        std::uint64_t executeFrame = 0;
        LogicCommitSequence sequence = 0;
        std::uint32_t executionOrdinalExclusive = 0;
        ActionOrderIdentity preparedIssue{};
        bool pendingPlacement = false;
        bool finalPlacementCaptured = false;
        bool actionOrderConsumed = false;
        std::array<float, kVariableDefinitionCapacity> baselineValues{};
        // Each set bit owns an observer, so the slot cannot recycle while retained.
        CandidatePins candidatePins{};
        bool candidateAdmissionFailed = false;
        std::uint32_t valueCount = 0;
        std::uint32_t sourceCount = 0;
        std::uint32_t publishedSourceCount = 0;
        bool needsRebuild = false;
        std::atomic<bool> preparationFailed{false};
        std::atomic<VariableNativeStartReceipt> nativeReceipt{VariableNativeStartReceipt::None};
        bool nativeCalendarRequired = false;
        std::uint64_t nativeCalendarReservationSequence = 0;
        std::atomic<bool> nativeCalendarConsumed{false};
        VnVoiceHandle nativeHandle = 0;
        std::uint32_t timelineUseCount = 0;
        bool timelineOwnershipTransferred = false;
        std::atomic<EntryReadUseState> entryUse{EntryReadUseState::None};
        FrontierActivationDescriptor entryDescriptor{};
        DecisionTable* entryDecisions = nullptr;
        ScheduledTransactionPool* entryPool = nullptr;
        DecisionFuturePin entryPin;
        std::array<ReadCutValueRow, kVariableDefinitionCapacity> values{};
        std::array<HistoryRef, kVariableDefinitionCapacity> sources{};
        std::array<VariableSliceSource, kVariableDefinitionCapacity> publishedSources{};
    };

    ReadCutRow* ResolveReadCut(VariableReadCutRef cut) noexcept;
    bool CaptureReadCutImpl(VnEmitterHandle emitter, std::uint64_t executeFrame,
        LogicCommitSequence sequence, std::uint32_t executionOrdinalExclusive,
        VariableReadCutRef* out, const ActionOrderIdentity* privateIssue,
        bool pendingPlacement = false) noexcept;
    bool RebuildPendingReadCut(ReadCutRow& cut, std::uint64_t logicalFrame) noexcept;
    void RegisterCommittedPendingReadCutCandidate(std::uint32_t slot) noexcept;
    bool ValidateReadCutPublication(VariableReadCutRef cut, std::uint64_t finalFrame,
        bool* outWaiting) noexcept;
    bool BeginUnborrowedPendingVariableGroupCut(Engine& engine, EngineActionPlayScratch& producer,
        VariableReadCutRef ref, std::uint64_t anchor, VariableReadCutReader* out) noexcept;
    bool ReleaseFrontierEntryReadUse(ReadCutRow& cut) noexcept;
    bool FrontierEntryReadUseDetached(const ReadCutRow& cut, std::uint64_t completedAudioSequence) const noexcept;
    void PrepareClaimedSlicesImpl(Engine& engine, bool allowUnsettledFrontier, bool publishPages,
        const ActionOrderIdentity* privateIssue = nullptr);
    std::uint64_t pendingPlaybackPublicationThrough_ = 0;
    const ReadCutRow* ResolveReadCut(VariableReadCutRef cut) const noexcept;
    const ReadCutRow* ResolveNativeReadCut(const VariableNativeStartIdentity& identity) const noexcept;
    enum class TimelineReadUseState : std::uint8_t { Free, Prepared, Published, Quarantined };
    enum class TimelineInitializerState : std::uint8_t { None, PendingPlacement, Building, Staged, Frozen, Consumed };
    static constexpr std::uint32_t kTimelineInitialCommandCapacity = kBuiltinTargetCount + 4u;
    struct TimelineReadUseRow {
        std::atomic<std::uint32_t> generation{1};
        std::atomic<TimelineReadUseState> state{TimelineReadUseState::Free};
        VariableReadCutRef cut{};
        VnVoiceHandle nativeHandle = 0;
        TimelineArmTable::VariableReadLifetime lifetime;
        OriginalSourceSubmissionRegistry* sourceOwner = nullptr;
        OriginalSourceLifetimePin originalSource;
        std::atomic<TimelineInitializerState> initializer{TimelineInitializerState::None};
        std::uint64_t logicalFrame = 0;
        std::uint64_t onsetFrame = 0;
        std::uint64_t authoredStartOffset = 0;
        std::uint32_t initialCommandCount = 0;
        std::array<CommandRecord, kTimelineInitialCommandCapacity> initialCommands{};
    };
    bool BeginPendingTimelineReadUsePlacement(TimelineReadUseRow& use,
        std::uint64_t logicalFrame, std::uint64_t onsetFrame, VariableReadCutRef* cut) noexcept;
    bool PreparedPendingVariableGroupUsesMatch(const EngineActionPlayScratch& child,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement) const noexcept;
    static bool TimelineInitializerCommandsMatch(VnVoiceHandle nativeHandle,
        std::span<const CommandRecord> commands) noexcept;
    bool CaptureTimelineInitializerProof(VariableTimelineReadUseRef use, VnVoiceHandle nativeHandle,
        VariableTimelineInitializerProof* out) noexcept;
    TimelineReadUseRow* ResolveTimelineReadUse(VariableTimelineReadUseRef use) noexcept;
    const TimelineReadUseRow* ResolveTimelineReadUse(VariableTimelineReadUseRef use) const noexcept;
    void ReleaseTimelineReadUse(TimelineReadUseRow& use) noexcept;
    WriteRow* ResolveHistory(HistoryRef ref) noexcept;
    bool PreparedSourceDependsOn(VariableSliceRef source, VariableSliceRef cancelled) const noexcept;
    bool PinHistorySlice(WriteRow& row) noexcept;
    bool RecycleHistoryRow(std::uint32_t slot) noexcept;
    void ReclaimHistoryRows() noexcept;
    void ObserveAppliedWriteForReadCuts(const WriteRow& row) noexcept;

    struct GatheredTarget {
        VariableTargetLocator locator{};
        std::uint32_t revision = 0;
    };

    static bool SameFormat(const WorldFormatIdentityV1& a,
                           const WorldFormatIdentityV1& b) noexcept;
    static bool SameDefinition(const VariableDefinitionV1& a,
                               const VariableDefinitionV1& b) noexcept;
    static bool SameDefinition(const DefinitionRow& a,
                               const DefinitionRow& b) noexcept;
    static bool ValidDefinition(const VariableDefinitionV1& definition) noexcept;
    static bool ScopeEqual(const VariableScopeKey& a,
                           const VariableScopeKey& b) noexcept;
    static bool RowKeyEqual(const WriteRow& a, const WriteRow& b) noexcept;
    static bool Precedes(const WriteRow& a, const WriteRow& b) noexcept;
    static bool PublicationPrecedes(const WriteRow& a, const WriteRow& b) noexcept;
    static bool SameBankKey(const WriteRow& a, const WriteRow& b) noexcept;
    static bool SameServiceDependency(const WriteRow& a,
                                      const WriteRow& b) noexcept;
    static bool BatchSupersedeKeyIntersect(const WriteRow& a,
                                           const WriteRow& b) noexcept;
    static bool SameTarget(const VariableTargetLocator& a,
                           const VariableTargetLocator& b) noexcept;
    static bool SamePhysicalTarget(const VariableTargetLocator& a,
                                   const VariableTargetLocator& b) noexcept;
    static bool ExactStepValue(float value) noexcept;

    DefinitionRow* FindDefinition(VnVariableId id) noexcept;
    const DefinitionRow* FindDefinition(VnVariableId id) const noexcept;
    EmitterBank* FindEmitterBank(const VariableScopeKey& scope) noexcept;
    const EmitterBank* FindEmitterBank(const VariableScopeKey& scope) const noexcept;
    EmitterBank* EnsureEmitterBank(const VariableScopeKey& scope) noexcept;
    OverrideRow* FindOverride(EmitterBank& bank, VnVariableId id) noexcept;
    const OverrideRow* FindOverride(const EmitterBank& bank,
                                    VnVariableId id) const noexcept;
    const OverrideRow* FindCurrentOverride(const EmitterBank& bank,
                                           VnVariableId id) const noexcept;
    OverrideRow* EnsureOverride(EmitterBank& bank, VnVariableId id) noexcept;

    VariableAcceptResult Accept(WriteOperation operation,
                                const VariableScopeKey& scope,
                                VnVariableId id, float value,
                                const VariableCallObservation& observed,
                                VariableWriterIdentityV1 writer =
                                    VariableWriterIdentityV1::Game);
    VariableAcceptResult AcceptBatch(
        WriteOperation operation, const VariableScopeKey& scope,
        const VnVariableWriteV1* writes, std::uint32_t writeCount,
        const VariableCallObservation& observed,
        VariableWriterIdentityV1 writer,
        const EmitterTable* emitters,
        bool wholeBatchSupersede = true);
    static std::uint32_t MapPublicOutcome(VariableOutcomeKind outcome) noexcept;
    void RememberAcceptedTicket(LogicCommitSequence sequence,
                                WorldGeneration world,
                                TransportEpoch transport) noexcept;
    void RememberFinalizedTicket(LogicCommitSequence sequence,
                                 VariableOutcomeKind outcome,
                                 WorldGeneration world, TransportEpoch transport) noexcept;
    const TicketQueryRecord* FindTicketQuery(
        LogicCommitSequence sequence, WorldGeneration world,
        TransportEpoch transport) const noexcept;
    bool TicketExpired(const VnVariableTicketV1& ticket) const noexcept;
    void PushExpiredTicket(const TicketQueryRecord& record) noexcept;
    void SupersedeIntersectingPendingBatches(
        const PreparedVariableBatchToken& batch,
        LogicCommitSequence sequence) noexcept;
    bool CanSupersede(WriteRow& prior, const WriteRow& next,
                      LogicCommitSequence sequence) noexcept;
    VariableAcceptResult Prepare(
        WriteOperation operation, const VariableScopeKey& scope,
        VnVariableId id, float value,
        const VariableCallObservation& observed,
        std::uint32_t executionOrdinal,
        PreparedVariableWriteToken* out,
        VariableWriterIdentityV1 writer = VariableWriterIdentityV1::Game,
        bool staging = false) noexcept;
    void CommitPreparedWriteImpl(PreparedVariableWriteToken* prepared, LogicCommitSequence sequence,
        const LogicCommitCoordinator::ConsumedIssue* consumed,
        const ActionFiringTable::ChildActivationReceipt* activated = nullptr,
        bool supersede = true) noexcept;
    bool CanTerminateForWorldReplacement(const WriteRow& row) const noexcept;
    void FinalizeRow(WriteRow& row, VariableOutcomeKind outcome,
                     LogicCommitTicket related, std::uint64_t actualFrame) noexcept;
    std::array<WriteRow*, kVariableBatchWriteCapacity> BatchRows(WriteRow& row) noexcept;
    void SyncBatchState(WriteRow& leader) noexcept;
    void FinalizeBatch(WriteRow& leader, VariableOutcomeKind outcome,
                       LogicCommitTicket related, std::uint64_t actualFrame) noexcept;
    void ReleaseEmitterOverrideReservation(WriteRow& row) noexcept;
    template <typename Visitor>
    void ForEachTarget(Engine& engine, WriteRow& row, Visitor&& visitor) noexcept;
    bool SharesPhysicalTargets(Engine& engine, WriteRow& prior,
                              const WriteRow& row) noexcept;
    static bool ScopeAppliesToTarget(const Engine& engine, const WriteRow& row,
                                    const VariableTargetLocator& target) noexcept;
    bool CaptureCandidateSources(Engine& engine, WriteRow& row,
                                 WriteRow* const* prefix, std::size_t count) noexcept;
    bool GatherTargets(Engine& engine, WriteRow& row,
                       std::uint32_t* outCount) noexcept;
    bool RecensusMatches(Engine& engine, WriteRow& row) noexcept;
    void ProjectCandidate(Engine& engine, const WriteRow& row);
    void ProjectCurrentApplied(Engine& engine, const WriteRow& row);
    static void RunVariableEvaluators(Engine& engine);
    enum class CandidateCaptureResult : std::uint8_t {
        Ready,
        Retry,
        Rejected
    };
    CandidateCaptureResult CaptureCandidateSlice(Engine& engine, WriteRow& row);
    void RejectPreparedRow(Engine& engine, WriteRow& row) noexcept;
    bool SnapshotCandidateBaselines(Engine& engine, WriteRow& row) noexcept;
    void ClearCandidateBaselines(Engine& engine, WriteRow& row) noexcept;
    void SetSliceRampSuppressed(Engine& engine, const WriteRow& row,
                                bool suppressed) noexcept;
    void ResetPreparedRow(Engine& engine, WriteRow& row) noexcept;
    bool PrepareCurrentSlice(Engine& engine, WriteRow& row);
    void ReleaseSliceClaims(
        Engine& engine, WriteRow& row,
        ClaimReleaseReason reason = ClaimReleaseReason::RetryPreserve) noexcept;
    void CommitSliceTargets(Engine& engine, WriteRow& row) noexcept;
    void CommitAppliedBank(const WriteRow& row);
    bool SemanticIdentical(const ContainerWorldInstallViewV1& install) const;
    WorldInstallResult InstallImpl(const ContainerWorldInstallViewV1& install,
                                   std::uint32_t replayVintage,
                                   bool replacementAdmitted);
    WorldInstallResult InstallHistoricalReplay(
        const HistoricalContainerWorldViewV1& install,
        std::uint32_t replayVintage);

    std::uint32_t maxEmitters_ = 0;
    std::uint32_t overridesPerEmitter_ = 0;
    const std::uint32_t maxActionDefinitions_ = 0;
    std::vector<GatheredTarget> censusScratch_;
    bool hasWorld_ = false;
    WorldIdentityV1 identity_{};
    WorldCapacitySummaryV1 capacity_{};
    WorldGeneration worldGeneration_ = 0;
    LogicCommitCoordinator& commits_;
    TransportEpochOwner& transport_;
    VariableSliceTable& slices_;
    ScheduledTransactionPool& timeline_;
    std::vector<DefinitionRow> definitions_;
    std::vector<VariableDefinitionV1> definitionViews_;
    std::vector<OwnedPropertyProgram> playableProperties_;
    std::vector<PlayablePropertyProgramViewV1> playablePropertyViews_;
    std::vector<OwnedOccurrenceProgram> playableOccurrences_;
    std::vector<PlayableOccurrencePropertyViewV1> playableOccurrenceViews_;
    std::vector<OwnedTrackScheduleProgram> trackSchedules_;
    std::vector<TrackScheduleProgramViewV1> trackScheduleViews_;
    std::vector<OwnedPieceScheduleProgram> pieceSchedules_;
    std::vector<PieceScheduleProgramViewV1> pieceScheduleViews_;
    std::vector<OwnedContainerProgram> containerPrograms_;
    std::vector<ContainerProgramViewV3> containerProgramViews_;
    std::vector<OwnedTransitionProgram> transitionPrograms_;
    std::vector<TransitionProgramViewV1> transitionProgramViews_;
    std::vector<OwnedActionProgram> actionPrograms_;
    std::vector<ActionProgramViewV1> actionProgramViews_;
    std::vector<EmitterBank> emitterBanks_;
    std::vector<std::uint32_t> emitterBankVersionGenerations_;
    // History reuse requires outcome delivery, zero observers and an audio fence.
    std::vector<WriteRow> history_;
    std::vector<HeldCurrentSliceToken> preparedWritePlacements_;
    std::vector<std::uint32_t> historyGenerations_;
    std::vector<std::uint32_t> freeSlots_;
    std::uint32_t revisionCapacity_ = 0;
    std::uint32_t queryHistoryCapacity_ = 0;
    std::uint32_t completionCapacity_ = 0;
    std::uint32_t applyPayloadCapacity_ = 0;
    std::uint32_t occupiedRevisions_ = 0;
    std::vector<WriteRow*> sealRows_, commitRows_, prefixRows_, readCutRows_, actionReadCutRows_;
    std::vector<WriteRow*> preparationRows_, settledRows_, supersedeRows_;
    std::vector<std::size_t> serviceCandidates_, preparationOrder_;
    std::vector<HeldCurrentSliceToken> preparationHeld_;
    mutable std::vector<VariableSliceRef> dependencyVisited_;
    std::uint32_t readCutCapacity_ = 0;
    std::unique_ptr<ReadCutRow[]> readCuts_;
    std::vector<std::uint64_t> candidatePinStorage_;
    std::uint32_t timelineReadUseCapacity_ = 0;
    std::unique_ptr<TimelineReadUseRow[]> timelineReadUses_;
    // Terminal records are delivered after each completed logic operation.
    std::vector<VariableOutcomeRecord> outcomes_;
    std::vector<TicketQueryRecord> ticketQueryRecords_;
    std::uint32_t ticketQueryBegin_ = 0;
    std::uint32_t ticketQueryCount_ = 0;
    std::vector<VnVariableTicketV1> expiredTicketRecords_;
    std::uint32_t expiredTicketBegin_ = 0;
    std::uint32_t expiredTicketCount_ = 0;
    std::size_t outcomeRead_ = 0;
    std::size_t deliveredOutcomeCount_ = 0;
    std::uint64_t currentOutcomeGeneration_ = 1;
    std::uint64_t deliveredOutcomeGeneration_ = 0;
    std::uint64_t pendingRows_ = 0;
    std::uint64_t preparedRows_ = 0;
    std::uint64_t nextBankOrdinal_ = 1;
    TargetClaimOwner publicationOwner_{};
    WorldGeneration publicationWorld_ = 0;
    TransportEpoch publicationTransport_ = 0;
    std::uint32_t globalBankVersionGeneration_ = 1;
    WorldDrainState drainState_ = WorldDrainState::Idle;
    bool engineAdmissionClosed_ = false;
    TransportDrainState transportDrainState_ = TransportDrainState::Idle;
    std::uint64_t transportDrainEpoch_ = 0;
    std::uint64_t transportDrainFrame_ = 0;
    std::size_t serviceCursor_ = 0;
    std::uint64_t observedBlock_ = 0;
    std::uint64_t serviceAudioFrame_ = 0;
    std::uint64_t acceptedCount_ = 0;
    std::uint64_t appliedCount_ = 0;
    std::uint64_t supersededCount_ = 0;
    std::uint64_t terminationCount_ = 0;
    std::uint64_t historyHighWater_ = 0;
    std::uint64_t refusalCount_ = 0;
    std::uint64_t sdkCatalogFingerprint_ = 0;
    std::vector<VariableLabelCatalogV1> labelCatalog_;
};

enum class CurrentWorldTraceOwnerV1 : std::uint8_t {
    Internal = 0,
    OuterCall98 = 1,
};

WorldInstallResult InstallContainerWorldV1(
    Engine& engine, const ContainerWorldInstallViewV1& install,
    CurrentWorldTraceOwnerV1 traceOwner =
        CurrentWorldTraceOwnerV1::Internal) noexcept;
WorldInstallResult InstallContainerWorldV2Replay(
    Engine& engine, const HistoricalContainerWorldViewV1& install) noexcept;
WorldInstallResult InstallContainerWorldV3Replay(
    Engine& engine, const HistoricalContainerWorldViewV1& install) noexcept;
WorldInstallResult InstallContainerWorldV4Replay(
    Engine& engine, const HistoricalContainerWorldViewV1& install) noexcept;
WorldInstallResult InstallContainerWorldV1(
    const ContainerWorldInstallViewV1& install) noexcept;

} // namespace harken

#endif // HARKEN_PARAM_VARIABLE_SERVICE_H
