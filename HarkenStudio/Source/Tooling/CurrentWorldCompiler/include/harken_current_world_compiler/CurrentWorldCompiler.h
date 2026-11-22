#ifndef HARKEN_CURRENT_WORLD_COMPILER_H
#define HARKEN_CURRENT_WORLD_COMPILER_H

#include <harken_studio/ObjectId.h>
#include "harken_studio_build/World.h"
#include "harken_studio_build/VariableDefinition.h"
#include "harken_project_schema/Model.h"
#include <harken_bus_compiler/BusCompiler.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace harkencurrentworldcompiler {



struct DuckingSendAuthoringV1 {
    harken::studio::types::ObjectId stableId{};
    harken::studio::build::SignalEndpointKindV1 sourceKind = harken::studio::build::SignalEndpointKindV1::Sound;
    harken::studio::types::ObjectId sourceId{};
    harken::studio::types::ObjectId targetBusId{};
    float sendGainDb = 0;
};

struct DefinitionMaterializationV1 {
    harkenplayableproperties::ContentPropertyKindV1 kind =
        harkenplayableproperties::ContentPropertyKindV1::Sound;
    harken::studio::types::ObjectId stableId{};
    std::uint64_t contentRevision = 0;
    harken::studio::types::ObjectId routeStableId{};
    std::uint64_t compatibilityFingerprint = 0;
};


struct CompletionLeafAuthoringV1 {
    harken::studio::build::AuthoredCompletionLeafV1 value{};
    std::vector<harken::studio::build::StableAuthoredPathStepV1> path;
};

struct CompletionAuthoringV1 {
    harken::studio::build::AuthoredCompletionClassV1 completionClass =
        harken::studio::build::AuthoredCompletionClassV1::SignalOnly;
    std::vector<CompletionLeafAuthoringV1> leaves;
};

struct ContainerChildAuthoringV1 {
    std::uint32_t stableOccurrenceOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t frequencyWeight = 0;
    std::uint32_t orderWeight = 0;
    harken::studio::build::QueueStopPolicyV1 queueStopPolicy =
        harken::studio::build::QueueStopPolicyV1::AbortOwningQueue;
    CompletionAuthoringV1 completion{};
};

struct DiscreteGridRowAuthoringV1 {
    std::uint32_t stableRowOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t childStableOrdinal = 0;
    std::vector<harken::studio::build::DiscreteGridCellV1> cells;
};

struct ContinuousGridRowAuthoringV1 {
    std::uint32_t stableRowOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t childStableOrdinal = 0;
    std::vector<harken::studio::build::ContinuousGridCellV1> cells;
};

struct ContainerAuthoringV1 {
    harken::studio::types::ObjectId containerStableId{};
    harken::studio::build::ContainerModeV1 mode = harken::studio::build::ContainerModeV1::Random;
    harken::studio::build::RandomDisciplineV1 randomDiscipline =
        harken::studio::build::RandomDisciplineV1::Draw;
    harken::studio::build::MemoryScopeV1 memoryScope = harken::studio::build::MemoryScopeV1::Emitter;
    harken::studio::build::RetriggerPolicyV1 retriggerPolicy =
        harken::studio::build::RetriggerPolicyV1::CreateNewInstance;
    harken::studio::build::ResetPolicyV1 resetPolicy =
        harken::studio::build::ResetPolicyV1::ExplicitTypedResetOnly;
    harken::studio::build::SequenceEndPolicyV1 sequenceEndPolicy =
        harken::studio::build::SequenceEndPolicyV1::Wrap;
    harken::studio::build::QueueCycleV1 queueCycle = harken::studio::build::QueueCycleV1::Once;
    std::uint32_t queueCycleCount = 0;
    harken::studio::build::QueueSpliceV1 queueSplice = harken::studio::build::QueueSpliceV1::Butt;
    bool resumePosition = false;
    std::uint32_t avoidLastN = 0;
    std::uint32_t initialSequenceCursor = 0;
    std::uint32_t discreteTiming = 0;
    std::uint32_t continuousTiming = 1;
    std::uint32_t continuousNormalization = 0;
    std::uint32_t evaluationPeriodFrames = 0;
    std::uint32_t envelopeFrames = 0;
    std::uint32_t maxActiveRows = 0;
    std::uint32_t maxLiveRows = 0;
    std::uint32_t transitionProgramIndex = UINT32_MAX;
    std::uint32_t authoredStableOrder = 0;
    std::vector<harken::studio::build::CurveV1> curves;
    std::vector<harken::studio::build::GridAxisV1> discreteAxes;
    std::vector<DiscreteGridRowAuthoringV1> discreteRows;
    std::vector<harken::studio::build::GridAxisV1> continuousAxes;
    std::vector<ContinuousGridRowAuthoringV1> continuousRows;
    std::vector<ContainerChildAuthoringV1> children;
};

struct TransitionProgramAuthoringV1 {
    harken::studio::types::ObjectId ownerContainerStableId{};
    std::uint32_t defaultRuleIndex = 0;
    std::uint32_t maxContainerDescent = 0;
    std::uint32_t maxOwnerAscent = 0;
    std::uint32_t watchFallback = 0;
    std::vector<harken::studio::build::StableAuthoredPathStepV1> downwardPathSteps;
    std::vector<harken::studio::build::TransitionRuleViewV1> rules;
};

struct ActionObjectRefAuthoringV1 {
    harken::studio::build::ActionTargetKindV1 kind = harken::studio::build::ActionTargetKindV1::Sound;
    harken::studio::types::ObjectId stableId{};
};

struct ActionStepAuthoringV1 {
    harken::studio::build::ActionOpcodeV1 opcode = harken::studio::build::ActionOpcodeV1::Play;
    std::uint32_t authoredEventIndex = 0;
    ActionObjectRefAuthoringV1 target{};
    float delayMilliseconds = 0.0f;
    float fadeMilliseconds = 0.0f;
    ActionObjectRefAuthoringV1 quantizeReference{};
    harken::studio::build::ActionQuantizeTimingV1 quantizeTiming =
        harken::studio::build::ActionQuantizeTimingV1::Immediate;
    harken::studio::build::ActionCooldownScopeV1 cooldownScope =
        harken::studio::build::ActionCooldownScopeV1::Global;
    harken::studio::build::ActionIfSilentV1 ifSilent = harken::studio::build::ActionIfSilentV1::Immediate;
    harken::studio::build::ActionScopeV1 resetScope = harken::studio::build::ActionScopeV1::Emitter;
    harken::studio::build::ActionScopeV1 scope = harken::studio::build::ActionScopeV1::Emitter;
    float cooldownSeconds = 0.0f;
    float value = 0.0f;
};

struct ActionProgramAuthoringV1 {
    harken::studio::types::ObjectId actionStableId{};
    std::vector<ActionStepAuthoringV1> steps;
};

enum class CompileStatusV1 : std::uint32_t {
    Ok = 0,
    InvalidProject = 1,
    MissingMaterialization = 2,
    InvalidMaterialization = 3,
    MissingContainerAuthoring = 4,
    InvalidContainerAuthoring = 5,
    Capacity = 6,
    InvalidCanonicalWorld = 7,
    TransitionCapabilityUnavailable = 8,
    InvalidActionAuthoring = 9,
};

class OwningCurrentWorldV1;
struct CompileResultV1;

enum class CompileCapacityModeV1 {
    ExplicitBudget,
    AuthoredContent,
};

class OwningCurrentWorldV1 {
public:
    OwningCurrentWorldV1() = default;
    OwningCurrentWorldV1(const OwningCurrentWorldV1&) = delete;
    OwningCurrentWorldV1& operator=(const OwningCurrentWorldV1&) = delete;
    OwningCurrentWorldV1(OwningCurrentWorldV1&&) = delete;
    OwningCurrentWorldV1& operator=(OwningCurrentWorldV1&&) = delete;

    const harken::studio::build::ContainerWorldInstallViewV1& View() const noexcept {
        return view_;
    }

private:
    friend CompileResultV1 CompileCurrentWorldV1(
        const harken::studio::project_schema::CanonicalProject&,
        const std::vector<harken::studio::build::VariableDefinitionV1>&,
        const std::vector<DefinitionMaterializationV1>&,
        const std::vector<ContainerAuthoringV1>&,
        const harken::studio::build::WorldCapacitySummaryV1&,
        const std::vector<TransitionProgramAuthoringV1>&,
        const std::vector<ActionProgramAuthoringV1>&,
        const harken::studio::bus_compiler::CompiledBusIdSetV1*,
        CompileCapacityModeV1,
        const std::map<harken::studio::types::ObjectId, std::uint32_t>&,
        const std::set<std::uint32_t>&,
        const std::vector<DuckingSendAuthoringV1>&, bool);

    struct OwnedPayload {
        harken::studio::build::PlayablePropertyPayloadViewV1 view{};
        std::vector<harken::studio::build::SendV1> sends;
        std::vector<harken::studio::build::EffectNodeV1> effectNodes;
        std::vector<std::vector<harken::studio::build::EffectParamV1>> effectParams;
        std::vector<harken::studio::build::EffectEdgeV1> effectEdges;
        void Refresh() noexcept;
    };

    struct OwnedProperty {
        harken::studio::build::PlayablePropertyProgramViewV1 view{};
        std::vector<harken::studio::build::LayerAncestorRefV1> ancestors;
        std::vector<harken::studio::build::StructuralAncestorRefV1> structuralAncestors;
        OwnedPayload payload;
        void Refresh() noexcept;
    };

    struct OwnedOccurrence {
        harken::studio::build::PlayableOccurrencePropertyViewV1 view{};
        OwnedPayload payload;
        void Refresh() noexcept;
    };

    struct OwnedContainer {
        harken::studio::build::ContainerProgramViewV3 view{};
        std::vector<harken::studio::build::CurveV1> curves;
        std::vector<harken::studio::build::ContainerChildProgramViewV2> children;
        std::vector<harken::studio::build::GridAxisV1> discreteAxes;
        std::vector<harken::studio::build::DiscreteGridRowViewV1> discreteRows;
        std::vector<std::vector<harken::studio::build::DiscreteGridCellV1>> discreteCells;
        std::vector<harken::studio::build::GridAxisV1> continuousAxes;
        std::vector<harken::studio::build::ContinuousGridRowViewV1> continuousRows;
        std::vector<std::vector<harken::studio::build::ContinuousGridCellV1>> continuousCells;
        void Refresh() noexcept;
    };

    struct OwnedTrackSchedule {
        harken::studio::build::TrackScheduleProgramViewV1 view{};
        std::vector<harken::studio::build::ScheduleClipPlacementV1> placements;
        void Refresh() noexcept;
    };

    struct OwnedSegmentSchedule {
        harken::studio::build::SegmentScheduleProgramViewV1 view{};
        std::vector<harken::studio::build::ScheduleTrackRefV1> trackRefs;
        std::vector<harken::studio::build::ScheduleMarkerV1> markers;
        void Refresh() noexcept;
    };

    struct OwnedTransition {
        harken::studio::build::TransitionProgramViewV1 view{};
        std::vector<harken::studio::build::StableAuthoredPathStepV1> downwardPathSteps;
        std::vector<harken::studio::build::TransitionRuleViewV1> rules;
        void Refresh() noexcept;
    };

    struct OwnedAction {
        harken::studio::build::ActionProgramViewV1 view{};
        std::vector<harken::studio::build::ActionStepProgramViewV1> steps;
        void Refresh() noexcept;
    };

    void Refresh() noexcept;

    harken::studio::build::ContainerWorldInstallViewV1 view_{};
    std::vector<harken::studio::build::VariableDefinitionV1> variables_;
    std::vector<OwnedProperty> properties_;
    std::vector<harken::studio::build::PlayablePropertyProgramViewV1> propertyViews_;
    std::vector<OwnedOccurrence> occurrences_;
    std::vector<harken::studio::build::PlayableOccurrencePropertyViewV1> occurrenceViews_;
    std::vector<OwnedTrackSchedule> trackSchedules_;
    std::vector<harken::studio::build::TrackScheduleProgramViewV1> trackScheduleViews_;
    std::vector<OwnedSegmentSchedule> segmentSchedules_;
    std::vector<harken::studio::build::SegmentScheduleProgramViewV1> segmentScheduleViews_;
    std::vector<OwnedContainer> containers_;
    std::vector<harken::studio::build::ContainerProgramViewV3> containerViews_;
    std::vector<OwnedTransition> transitions_;
    std::vector<harken::studio::build::TransitionProgramViewV1> transitionViews_;
    std::vector<harken::studio::build::SignalRelationV1> signalRelations_;
    std::vector<harken::studio::build::SignalReceiverV1> signalReceivers_;
    std::vector<OwnedAction> actions_;
    std::vector<harken::studio::build::ActionProgramViewV1> actionViews_;
};

struct CompileResultV1 {
    CompileStatusV1 status = CompileStatusV1::InvalidProject;
    harken::studio::types::ObjectId relatedStableId{};
    std::unique_ptr<OwningCurrentWorldV1> world;

    bool ok() const noexcept { return status == CompileStatusV1::Ok; }
};

CompileResultV1 CompileCurrentWorldV1(
    const harken::studio::project_schema::CanonicalProject& project,
    const std::vector<harken::studio::build::VariableDefinitionV1>& variables,
    const std::vector<DefinitionMaterializationV1>& materializations,
    const std::vector<ContainerAuthoringV1>& containers,
    const harken::studio::build::WorldCapacitySummaryV1& capacity = {},
    const std::vector<TransitionProgramAuthoringV1>& transitions = {},
    const std::vector<ActionProgramAuthoringV1>& actions = {},
    const harken::studio::bus_compiler::CompiledBusIdSetV1* busIds = nullptr,
    CompileCapacityModeV1 capacityMode = CompileCapacityModeV1::ExplicitBudget,
    const std::map<harken::studio::types::ObjectId, std::uint32_t>& propertyAuthoredOrders = {},
    const std::set<std::uint32_t>& reservedPropertyAuthoredOrders = {},
    const std::vector<DuckingSendAuthoringV1>& duckingSends = {},
    bool includePreviewDucking = false);

} // namespace harkencurrentworldcompiler

#endif // HARKEN_CURRENT_WORLD_COMPILER_H
