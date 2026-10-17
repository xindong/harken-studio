#ifndef HARKEN_CURRENT_WORLD_COMPILER_H
#define HARKEN_CURRENT_WORLD_COMPILER_H

#include <harken_studio/ObjectId.h>
#include "harken_studio_build/World.h"
#include "harken_studio_build/VariableDefinition.h"
#include "harken_project_schema/Model.h"
#include <harken_bus_compiler/BusCompiler.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace harkencurrentworldcompiler {



struct DefinitionMaterializationV1 {
    harkenplayableproperties::ContentPropertyKindV1 kind =
        harkenplayableproperties::ContentPropertyKindV1::Sound;
    harkenstudio::ObjectId stableId{};
    std::uint64_t contentRevision = 0;
    harkenstudio::ObjectId routeStableId{};
    std::uint64_t compatibilityFingerprint = 0;
};


struct CompletionLeafAuthoringV1 {
    harkenstudiobuild::AuthoredCompletionLeafV1 value{};
    std::vector<harkenstudiobuild::StableAuthoredPathStepV1> path;
};

struct CompletionAuthoringV1 {
    harkenstudiobuild::AuthoredCompletionClassV1 completionClass =
        harkenstudiobuild::AuthoredCompletionClassV1::SignalOnly;
    std::vector<CompletionLeafAuthoringV1> leaves;
};

struct ContainerChildAuthoringV1 {
    std::uint32_t stableOccurrenceOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t frequencyWeight = 0;
    std::uint32_t orderWeight = 0;
    harkenstudiobuild::QueueStopPolicyV1 queueStopPolicy =
        harkenstudiobuild::QueueStopPolicyV1::AbortOwningQueue;
    CompletionAuthoringV1 completion{};
};

struct DiscreteGridRowAuthoringV1 {
    std::uint32_t stableRowOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t childStableOrdinal = 0;
    std::vector<harkenstudiobuild::DiscreteGridCellV1> cells;
};

struct ContinuousGridRowAuthoringV1 {
    std::uint32_t stableRowOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t childStableOrdinal = 0;
    std::vector<harkenstudiobuild::ContinuousGridCellV1> cells;
};

struct ContainerAuthoringV1 {
    harkenstudio::ObjectId containerStableId{};
    harkenstudiobuild::ContainerModeV1 mode = harkenstudiobuild::ContainerModeV1::Random;
    harkenstudiobuild::RandomDisciplineV1 randomDiscipline =
        harkenstudiobuild::RandomDisciplineV1::Draw;
    harkenstudiobuild::MemoryScopeV1 memoryScope = harkenstudiobuild::MemoryScopeV1::Emitter;
    harkenstudiobuild::RetriggerPolicyV1 retriggerPolicy =
        harkenstudiobuild::RetriggerPolicyV1::CreateNewInstance;
    harkenstudiobuild::ResetPolicyV1 resetPolicy =
        harkenstudiobuild::ResetPolicyV1::ExplicitTypedResetOnly;
    harkenstudiobuild::SequenceEndPolicyV1 sequenceEndPolicy =
        harkenstudiobuild::SequenceEndPolicyV1::Wrap;
    harkenstudiobuild::QueueCycleV1 queueCycle = harkenstudiobuild::QueueCycleV1::Once;
    std::uint32_t queueCycleCount = 0;
    harkenstudiobuild::QueueSpliceV1 queueSplice = harkenstudiobuild::QueueSpliceV1::Butt;
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
    std::vector<harkenstudiobuild::CurveV1> curves;
    std::vector<harkenstudiobuild::GridAxisV1> discreteAxes;
    std::vector<DiscreteGridRowAuthoringV1> discreteRows;
    std::vector<harkenstudiobuild::GridAxisV1> continuousAxes;
    std::vector<ContinuousGridRowAuthoringV1> continuousRows;
    std::vector<ContainerChildAuthoringV1> children;
};

struct TransitionProgramAuthoringV1 {
    harkenstudio::ObjectId ownerContainerStableId{};
    std::uint32_t defaultRuleIndex = 0;
    std::uint32_t maxContainerDescent = 0;
    std::uint32_t maxOwnerAscent = 0;
    std::uint32_t watchFallback = 0;
    std::vector<harkenstudiobuild::StableAuthoredPathStepV1> downwardPathSteps;
    std::vector<harkenstudiobuild::TransitionRuleViewV1> rules;
};

struct ActionObjectRefAuthoringV1 {
    harkenstudiobuild::ActionTargetKindV1 kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    harkenstudio::ObjectId stableId{};
};

struct ActionStepAuthoringV1 {
    harkenstudiobuild::ActionOpcodeV1 opcode = harkenstudiobuild::ActionOpcodeV1::Play;
    std::uint32_t authoredEventIndex = 0;
    ActionObjectRefAuthoringV1 target{};
    float delayMilliseconds = 0.0f;
    float fadeMilliseconds = 0.0f;
    ActionObjectRefAuthoringV1 quantizeReference{};
    harkenstudiobuild::ActionQuantizeTimingV1 quantizeTiming =
        harkenstudiobuild::ActionQuantizeTimingV1::Immediate;
    harkenstudiobuild::ActionCooldownScopeV1 cooldownScope =
        harkenstudiobuild::ActionCooldownScopeV1::Global;
    harkenstudiobuild::ActionIfSilentV1 ifSilent = harkenstudiobuild::ActionIfSilentV1::Immediate;
    harkenstudiobuild::ActionScopeV1 resetScope = harkenstudiobuild::ActionScopeV1::Emitter;
    harkenstudiobuild::ActionScopeV1 scope = harkenstudiobuild::ActionScopeV1::Emitter;
    float cooldownSeconds = 0.0f;
    float value = 0.0f;
};

struct ActionProgramAuthoringV1 {
    harkenstudio::ObjectId actionStableId{};
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
    EntryScheduleCapabilityUnavailable = 10,
};

class OwningCurrentWorldV1;
struct CompileResultV1;

class OwningCurrentWorldV1 {
public:
    OwningCurrentWorldV1() = default;
    OwningCurrentWorldV1(const OwningCurrentWorldV1&) = delete;
    OwningCurrentWorldV1& operator=(const OwningCurrentWorldV1&) = delete;
    OwningCurrentWorldV1(OwningCurrentWorldV1&&) = delete;
    OwningCurrentWorldV1& operator=(OwningCurrentWorldV1&&) = delete;

    const harkenstudiobuild::ContainerWorldInstallViewV1& View() const noexcept {
        return view_;
    }

private:
    friend CompileResultV1 CompileCurrentWorldV1(
        const harkenprojectschema::CanonicalProject&,
        const std::vector<harkenstudiobuild::VariableDefinitionV1>&,
        const std::vector<DefinitionMaterializationV1>&,
        const std::vector<ContainerAuthoringV1>&,
        const harkenstudiobuild::WorldCapacitySummaryV1&,
        const std::vector<TransitionProgramAuthoringV1>&,
        const std::vector<ActionProgramAuthoringV1>&,
        const harkenbuscompiler::CompiledBusIdSetV1*, std::uint32_t);

    struct OwnedPayload {
        harkenstudiobuild::PlayablePropertyPayloadViewV1 view{};
        std::vector<harkenstudiobuild::SendV1> sends;
        std::vector<harkenstudiobuild::EffectNodeV1> effectNodes;
        std::vector<std::vector<harkenstudiobuild::EffectParamV1>> effectParams;
        std::vector<harkenstudiobuild::EffectEdgeV1> effectEdges;
        void Refresh() noexcept;
    };

    struct OwnedProperty {
        harkenstudiobuild::PlayablePropertyProgramViewV1 view{};
        std::vector<harkenstudiobuild::LayerAncestorRefV1> ancestors;
        std::vector<harkenstudiobuild::StructuralAncestorRefV1> structuralAncestors;
        OwnedPayload payload;
        void Refresh() noexcept;
    };

    struct OwnedOccurrence {
        harkenstudiobuild::PlayableOccurrencePropertyViewV1 view{};
        OwnedPayload payload;
        void Refresh() noexcept;
    };

    struct OwnedContainer {
        harkenstudiobuild::ContainerProgramViewV3 view{};
        std::vector<harkenstudiobuild::CurveV1> curves;
        std::vector<harkenstudiobuild::ContainerChildProgramViewV2> children;
        std::vector<harkenstudiobuild::GridAxisV1> discreteAxes;
        std::vector<harkenstudiobuild::DiscreteGridRowViewV1> discreteRows;
        std::vector<std::vector<harkenstudiobuild::DiscreteGridCellV1>> discreteCells;
        std::vector<harkenstudiobuild::GridAxisV1> continuousAxes;
        std::vector<harkenstudiobuild::ContinuousGridRowViewV1> continuousRows;
        std::vector<std::vector<harkenstudiobuild::ContinuousGridCellV1>> continuousCells;
        void Refresh() noexcept;
    };

    struct OwnedTrackSchedule {
        harkenstudiobuild::TrackScheduleProgramViewV1 view{};
        std::vector<harkenstudiobuild::ScheduleClipPlacementV1> placements;
        void Refresh() noexcept;
    };

    struct OwnedPieceSchedule {
        harkenstudiobuild::PieceScheduleProgramViewV1 view{};
        std::vector<harkenstudiobuild::ScheduleTrackRefV1> trackRefs;
        std::vector<harkenstudiobuild::ScheduleMarkerV1> markers;
        void Refresh() noexcept;
    };

    struct OwnedTransition {
        harkenstudiobuild::TransitionProgramViewV1 view{};
        std::vector<harkenstudiobuild::StableAuthoredPathStepV1> downwardPathSteps;
        std::vector<harkenstudiobuild::TransitionRuleViewV1> rules;
        void Refresh() noexcept;
    };

    struct OwnedAction {
        harkenstudiobuild::ActionProgramViewV1 view{};
        std::vector<harkenstudiobuild::ActionStepProgramViewV1> steps;
        void Refresh() noexcept;
    };

    void Refresh() noexcept;

    harkenstudiobuild::ContainerWorldInstallViewV1 view_{};
    std::vector<harkenstudiobuild::VariableDefinitionV1> variables_;
    std::vector<OwnedProperty> properties_;
    std::vector<harkenstudiobuild::PlayablePropertyProgramViewV1> propertyViews_;
    std::vector<OwnedOccurrence> occurrences_;
    std::vector<harkenstudiobuild::PlayableOccurrencePropertyViewV1> occurrenceViews_;
    std::vector<OwnedTrackSchedule> trackSchedules_;
    std::vector<harkenstudiobuild::TrackScheduleProgramViewV1> trackScheduleViews_;
    std::vector<OwnedPieceSchedule> pieceSchedules_;
    std::vector<harkenstudiobuild::PieceScheduleProgramViewV1> pieceScheduleViews_;
    std::vector<OwnedContainer> containers_;
    std::vector<harkenstudiobuild::ContainerProgramViewV3> containerViews_;
    std::vector<OwnedTransition> transitions_;
    std::vector<harkenstudiobuild::TransitionProgramViewV1> transitionViews_;
    std::vector<OwnedAction> actions_;
    std::vector<harkenstudiobuild::ActionProgramViewV1> actionViews_;
};

struct CompileResultV1 {
    CompileStatusV1 status = CompileStatusV1::InvalidProject;
    harkenstudio::ObjectId relatedStableId{};
    std::uint32_t relatedEventIndex = UINT32_MAX;
    std::unique_ptr<OwningCurrentWorldV1> world;

    bool ok() const noexcept { return status == CompileStatusV1::Ok; }
};

CompileResultV1 CompileCurrentWorldV1(
    const harkenprojectschema::CanonicalProject& project,
    const std::vector<harkenstudiobuild::VariableDefinitionV1>& variables,
    const std::vector<DefinitionMaterializationV1>& materializations,
    const std::vector<ContainerAuthoringV1>& containers,
    const harkenstudiobuild::WorldCapacitySummaryV1& capacity = {},
    const std::vector<TransitionProgramAuthoringV1>& transitions = {},
    const std::vector<ActionProgramAuthoringV1>& actions = {},
    const harkenbuscompiler::CompiledBusIdSetV1* busIds = nullptr,
    std::uint32_t bodyVintage = harkenstudiobuild::kCurrentWorldShippingBodyVintage);

} // namespace harkencurrentworldcompiler

#endif // HARKEN_CURRENT_WORLD_COMPILER_H
