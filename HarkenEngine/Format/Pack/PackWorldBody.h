#ifndef HARKEN_PACK_WORLD_BODY_H
#define HARKEN_PACK_WORLD_BODY_H

#include "PackFile.h"

#define HK_PACK_BODY_WORLD_VARIABLE_HEADER_SCOPE 1u

#define HK_PACK_WORLD_CAPABILITY_VARIABLE_DEFINITIONS (UINT64_C(1) << 0)
#define HK_PACK_WORLD_CAPABILITY_PLAYABLE_PROPERTIES (UINT64_C(1) << 1)
#define HK_PACK_WORLD_CAPABILITY_RANDOM_DRAW (UINT64_C(1) << 2)
#define HK_PACK_WORLD_CAPABILITY_RANDOM_ROTATION (UINT64_C(1) << 3)
#define HK_PACK_WORLD_CAPABILITY_SEQUENCE (UINT64_C(1) << 4)
#define HK_PACK_WORLD_CAPABILITY_RANDOM_QUEUE (UINT64_C(1) << 5)
#define HK_PACK_WORLD_CAPABILITY_SEQUENCE_QUEUE (UINT64_C(1) << 6)
#define HK_PACK_WORLD_CAPABILITY_VARIABLE_CONTROL (UINT64_C(1) << 7)
#define HK_PACK_WORLD_CAPABILITY_TRANSITION (UINT64_C(1) << 8)
#define HK_PACK_WORLD_CAPABILITY_ACTION_DEFINITIONS (UINT64_C(1) << 9)
#define HK_PACK_WORLD_CAPABILITY_ENTRY_SCHEDULING (UINT64_C(1) << 10)
#define HK_PACK_WORLD_CAPABILITY_SIGNAL_RELATIONS (UINT64_C(1) << 11)

#define HK_PACK_WORLD_MAX_VARIABLE_DEFINITIONS 65536u
#define HK_PACK_WORLD_MAX_CONTAINER_PROGRAMS 65536u
#define HK_PACK_WORLD_MAX_CONTAINER_CHILDREN 1024u
#define HK_PACK_WORLD_MAX_SELECTION_AVOID 64u
#define HK_PACK_WORLD_MAX_PLAYABLE_PROPERTY_PROGRAMS 1048576u
#define HK_PACK_WORLD_MAX_PLAYABLE_OCCURRENCE_PROPERTIES 1048576u
#define HK_PACK_WORLD_MAX_PLAYABLE_PROPERTY_CHILD_ROWS 1048576u
#define HK_PACK_WORLD_MAX_PLAYABLE_PROPERTY_EFFECT_PARAMS 4194304u
#define HK_PACK_WORLD_MAX_PLAYABLE_PROPERTY_EFFECT_EDGES 2097152u
#define HK_PACK_WORLD_MAX_TRACK_SCHEDULE_PROGRAMS 1048576u
#define HK_PACK_WORLD_MAX_SEGMENT_SCHEDULE_PROGRAMS 1048576u
#define HK_PACK_WORLD_MAX_SCHEDULE_TRACK_REFS 1048576u
#define HK_PACK_WORLD_MAX_SCHEDULE_PLACEMENTS 1048576u
#define HK_PACK_WORLD_MAX_SCHEDULE_MARKERS 1048576u
#define HK_PACK_WORLD_MAX_ACTION_DEFINITIONS 65536u
#define HK_PACK_WORLD_MAX_ACTION_STEPS 4194304u
#define HK_PACK_WORLD_MAX_ACTION_EXPANDED_WORK 4194304u
#define HK_PACK_WORLD_MAX_STEPS_PER_ACTION 64u

#define HK_PACK_WORLD_SECTION_GROUP_PROPERTY_MASK UINT64_C(0x12fe0)

typedef struct HkPackBodyPayloadEffectEdge {
    uint32_t fromNodeId;
    uint32_t toNodeId;
} HkPackBodyPayloadEffectEdge;
HK_PACK_SIZE(HkPackBodyPayloadEffectEdge, 8);
HK_PACK_OFFSET(HkPackBodyPayloadEffectEdge, fromNodeId, 0);
HK_PACK_OFFSET(HkPackBodyPayloadEffectEdge, toNodeId, 4);

typedef struct HkPackBodyPayloadEffectNodeHeader {
    uint32_t nodeId;
    HkPackObjectId effectId;
    uint32_t effectType;
    uint32_t paramCount;
    uint32_t reserved;
} HkPackBodyPayloadEffectNodeHeader;
HK_PACK_SIZE(HkPackBodyPayloadEffectNodeHeader, 32);
HK_PACK_OFFSET(HkPackBodyPayloadEffectNodeHeader, nodeId, 0);
HK_PACK_OFFSET(HkPackBodyPayloadEffectNodeHeader, effectId, 4);
HK_PACK_OFFSET(HkPackBodyPayloadEffectNodeHeader, effectType, 20);
HK_PACK_OFFSET(HkPackBodyPayloadEffectNodeHeader, paramCount, 24);
HK_PACK_OFFSET(HkPackBodyPayloadEffectNodeHeader, reserved, 28);

typedef struct HkPackBodyPayloadEffectParam {
    uint32_t paramId;
    uint32_t valueBits;
} HkPackBodyPayloadEffectParam;
HK_PACK_SIZE(HkPackBodyPayloadEffectParam, 8);
HK_PACK_OFFSET(HkPackBodyPayloadEffectParam, paramId, 0);
HK_PACK_OFFSET(HkPackBodyPayloadEffectParam, valueBits, 4);

#pragma pack(push, 1)
typedef struct HkPackBodyPayloadHeader {
    uint64_t presentMask;
    uint32_t volume;
    uint32_t pitch;
    uint32_t outputBusVolume;
    uint32_t streamingMode;
    uint32_t loopCount;
    uint8_t spatialIs3DEnabled;
    uint16_t reserved;
    uint32_t spatialPanning;
    uint32_t spatialSpread;
    uint32_t spatialAttenuationDistanceScaling;
    uint32_t legacySpatialValue;
    HkPackObjectId outputBusRef;
    HkPackObjectId spatialAttenuationRef;
    uint64_t legacyValue;
    uint32_t spatializationMix;
    uint32_t sendCount;
    uint32_t reservedCount1;
    uint32_t reservedCount2;
    uint32_t effectNodeCount;
    uint32_t effectEdgeCount;
} HkPackBodyPayloadHeader;
#pragma pack(pop)
HK_PACK_SIZE(HkPackBodyPayloadHeader, 111);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, presentMask, 0);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, volume, 8);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, pitch, 12);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, outputBusVolume, 16);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, streamingMode, 20);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, loopCount, 24);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, spatialIs3DEnabled, 28);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, reserved, 29);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, spatialPanning, 31);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, spatialSpread, 35);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, spatialAttenuationDistanceScaling, 39);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, legacySpatialValue, 43);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, outputBusRef, 47);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, spatialAttenuationRef, 63);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, legacyValue, 79);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, spatializationMix, 87);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, sendCount, 91);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, reservedCount1, 95);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, reservedCount2, 99);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, effectNodeCount, 103);
HK_PACK_OFFSET(HkPackBodyPayloadHeader, effectEdgeCount, 107);

typedef struct HkPackBodyPayloadSend {
    uint32_t stableSendOrdinal;
    uint32_t reserved;
    HkPackObjectId targetBusId;
    float sendLevel;
    uint32_t reserved2;
} HkPackBodyPayloadSend;
HK_PACK_SIZE(HkPackBodyPayloadSend, 32);
HK_PACK_OFFSET(HkPackBodyPayloadSend, stableSendOrdinal, 0);
HK_PACK_OFFSET(HkPackBodyPayloadSend, reserved, 4);
HK_PACK_OFFSET(HkPackBodyPayloadSend, targetBusId, 8);
HK_PACK_OFFSET(HkPackBodyPayloadSend, sendLevel, 24);
HK_PACK_OFFSET(HkPackBodyPayloadSend, reserved2, 28);

typedef struct HkPackBodyWorldActionCapacity {
    uint32_t actionCount;
    uint32_t maxActionDefinitions;
    uint32_t maxActionStepsPerDefinition;
    uint32_t maxActionStepsTotal;
    uint32_t maxActionTriggerDepth;
    uint32_t maxActionExpandedWork;
} HkPackBodyWorldActionCapacity;
HK_PACK_SIZE(HkPackBodyWorldActionCapacity, 24);
HK_PACK_OFFSET(HkPackBodyWorldActionCapacity, actionCount, 0);
HK_PACK_OFFSET(HkPackBodyWorldActionCapacity, maxActionDefinitions, 4);
HK_PACK_OFFSET(HkPackBodyWorldActionCapacity, maxActionStepsPerDefinition, 8);
HK_PACK_OFFSET(HkPackBodyWorldActionCapacity, maxActionStepsTotal, 12);
HK_PACK_OFFSET(HkPackBodyWorldActionCapacity, maxActionTriggerDepth, 16);
HK_PACK_OFFSET(HkPackBodyWorldActionCapacity, maxActionExpandedWork, 20);

#pragma pack(push, 1)
typedef struct HkPackBodyWorldActionHeader {
    HkPackObjectId actionStableId;
    uint64_t programSemanticFingerprint;
    uint32_t stepCount;
    uint32_t directFeatureMask;
    uint32_t transitiveFeatureMask;
    uint32_t expandedStepOccurrenceCount;
    uint32_t expandedFiringCount;
} HkPackBodyWorldActionHeader;
#pragma pack(pop)
HK_PACK_SIZE(HkPackBodyWorldActionHeader, 44);
HK_PACK_OFFSET(HkPackBodyWorldActionHeader, actionStableId, 0);
HK_PACK_OFFSET(HkPackBodyWorldActionHeader, programSemanticFingerprint, 16);
HK_PACK_OFFSET(HkPackBodyWorldActionHeader, stepCount, 24);
HK_PACK_OFFSET(HkPackBodyWorldActionHeader, directFeatureMask, 28);
HK_PACK_OFFSET(HkPackBodyWorldActionHeader, transitiveFeatureMask, 32);
HK_PACK_OFFSET(HkPackBodyWorldActionHeader, expandedStepOccurrenceCount, 36);
HK_PACK_OFFSET(HkPackBodyWorldActionHeader, expandedFiringCount, 40);

typedef struct HkPackBodyWorldActionRef {
    uint32_t kind;
    uint32_t reserved;
    HkPackObjectId stableId;
    uint64_t expectedProgramSemanticFingerprint;
} HkPackBodyWorldActionRef;
HK_PACK_SIZE(HkPackBodyWorldActionRef, 32);
HK_PACK_OFFSET(HkPackBodyWorldActionRef, kind, 0);
HK_PACK_OFFSET(HkPackBodyWorldActionRef, reserved, 4);
HK_PACK_OFFSET(HkPackBodyWorldActionRef, stableId, 8);
HK_PACK_OFFSET(HkPackBodyWorldActionRef, expectedProgramSemanticFingerprint, 24);

#pragma pack(push, 1)
typedef struct HkPackBodyWorldTime {
    uint32_t domain;
    uint64_t numerator;
    uint32_t denominator;
} HkPackBodyWorldTime;
#pragma pack(pop)
HK_PACK_SIZE(HkPackBodyWorldTime, 16);
HK_PACK_OFFSET(HkPackBodyWorldTime, domain, 0);
HK_PACK_OFFSET(HkPackBodyWorldTime, numerator, 4);
HK_PACK_OFFSET(HkPackBodyWorldTime, denominator, 12);

typedef struct HkPackBodyWorldActionStep {
    uint32_t opcode;
    uint32_t authoredEventIndex;
    HkPackBodyWorldActionRef target;
    HkPackBodyWorldTime delay;
    HkPackBodyWorldTime fade;
    HkPackBodyWorldActionRef quantizeReference;
    uint32_t quantizeTiming;
    uint32_t cooldownScope;
    uint32_t ifSilent;
    uint32_t scope;
    HkPackBodyWorldTime cooldown;
    uint32_t valueBits;
    uint32_t reserved0;
} HkPackBodyWorldActionStep;
HK_PACK_SIZE(HkPackBodyWorldActionStep, 144);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, opcode, 0);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, authoredEventIndex, 4);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, target, 8);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, delay, 40);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, fade, 56);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, quantizeReference, 72);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, quantizeTiming, 104);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, cooldownScope, 108);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, ifSilent, 112);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, scope, 116);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, cooldown, 120);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, valueBits, 136);
HK_PACK_OFFSET(HkPackBodyWorldActionStep, reserved0, 140);

typedef struct HkPackBodyWorldCapacity {
    uint32_t maxVariableDefinitions;
    uint32_t maxContainerPrograms;
    uint32_t maxPlayablePropertyPrograms;
    uint32_t maxPlayableOccurrencePropertyRows;
    uint32_t maxPlayablePropertySendsTotal;
    uint32_t maxPlayablePropertyLayerAncestorRefsTotal;
    uint32_t maxPlayablePropertyEffectNodesTotal;
    uint32_t maxPlayablePropertyEffectParamsTotal;
    uint32_t maxPlayablePropertyEffectEdgesTotal;
    uint32_t maxContainerDepth;
    uint32_t maxChildrenPerContainer;
    uint32_t maxAvoidLastN;
    uint32_t maxDiscreteAxes;
    uint32_t maxDiscreteRows;
    uint32_t maxDiscreteTrieNodes;
    uint32_t maxContinuousAxes;
    uint32_t maxContinuousRows;
    uint32_t maxContinuousCandidates;
    uint32_t maxContinuousActiveRows;
    uint32_t maxContinuousLiveRows;
    uint32_t maxTransitionRules;
    uint32_t maxCurvePoints;
    uint32_t maxKnownScheduleLeaves;
    uint32_t maxTrackSchedulePrograms;
    uint32_t maxSegmentSchedulePrograms;
    uint32_t maxScheduleTrackRefsPerSegment;
    uint32_t maxScheduleTrackRefsTotal;
    uint32_t maxSchedulePlacementsPerTrack;
    uint32_t maxSchedulePlacementsTotal;
    uint32_t maxScheduleMarkersPerSegment;
    uint32_t maxScheduleMarkersTotal;
    uint32_t maxTrackSegmentScheduleDepth;
    uint32_t maxActionDefinitions;
    uint32_t maxActionStepsPerDefinition;
    uint32_t maxActionStepsTotal;
    uint32_t maxActionTriggerDepth;
    uint32_t maxActionExpandedWork;
} HkPackBodyWorldCapacity;
HK_PACK_SIZE(HkPackBodyWorldCapacity, 148);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxVariableDefinitions, 0);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxContainerPrograms, 4);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxPlayablePropertyPrograms, 8);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxPlayableOccurrencePropertyRows, 12);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxPlayablePropertySendsTotal, 16);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxPlayablePropertyLayerAncestorRefsTotal, 20);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxPlayablePropertyEffectNodesTotal, 24);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxPlayablePropertyEffectParamsTotal, 28);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxPlayablePropertyEffectEdgesTotal, 32);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxContainerDepth, 36);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxChildrenPerContainer, 40);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxAvoidLastN, 44);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxDiscreteAxes, 48);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxDiscreteRows, 52);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxDiscreteTrieNodes, 56);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxContinuousAxes, 60);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxContinuousRows, 64);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxContinuousCandidates, 68);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxContinuousActiveRows, 72);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxContinuousLiveRows, 76);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxTransitionRules, 80);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxCurvePoints, 84);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxKnownScheduleLeaves, 88);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxTrackSchedulePrograms, 92);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxSegmentSchedulePrograms, 96);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxScheduleTrackRefsPerSegment, 100);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxScheduleTrackRefsTotal, 104);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxSchedulePlacementsPerTrack, 108);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxSchedulePlacementsTotal, 112);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxScheduleMarkersPerSegment, 116);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxScheduleMarkersTotal, 120);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxTrackSegmentScheduleDepth, 124);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxActionDefinitions, 128);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxActionStepsPerDefinition, 132);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxActionStepsTotal, 136);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxActionTriggerDepth, 140);
HK_PACK_OFFSET(HkPackBodyWorldCapacity, maxActionExpandedWork, 144);

typedef struct HkPackBodyWorldPlayableRef {
    uint32_t kind;
    uint32_t reserved;
    HkPackObjectId stableId;
    uint64_t expectedProgramSemanticFingerprint;
    uint64_t contentRevision;
    HkPackObjectId routeStableId;
    uint64_t compatibilityFingerprint;
} HkPackBodyWorldPlayableRef;
HK_PACK_SIZE(HkPackBodyWorldPlayableRef, 64);
HK_PACK_OFFSET(HkPackBodyWorldPlayableRef, kind, 0);
HK_PACK_OFFSET(HkPackBodyWorldPlayableRef, reserved, 4);
HK_PACK_OFFSET(HkPackBodyWorldPlayableRef, stableId, 8);
HK_PACK_OFFSET(HkPackBodyWorldPlayableRef, expectedProgramSemanticFingerprint, 24);
HK_PACK_OFFSET(HkPackBodyWorldPlayableRef, contentRevision, 32);
HK_PACK_OFFSET(HkPackBodyWorldPlayableRef, routeStableId, 40);
HK_PACK_OFFSET(HkPackBodyWorldPlayableRef, compatibilityFingerprint, 56);

typedef struct HkPackBodyWorldChildHeader {
    HkPackBodyWorldPlayableRef playable;
    uint32_t stableChildOrdinal;
    uint32_t authoredStableOrder;
    uint32_t frequencyWeight;
    uint32_t orderWeight;
    uint32_t queueStopPolicy;
    uint32_t reserved;
    uint32_t timingKind;
    uint32_t timingReserved;
} HkPackBodyWorldChildHeader;
HK_PACK_SIZE(HkPackBodyWorldChildHeader, 96);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, playable, 0);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, stableChildOrdinal, 64);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, authoredStableOrder, 68);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, frequencyWeight, 72);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, orderWeight, 76);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, queueStopPolicy, 80);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, reserved, 84);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, timingKind, 88);
HK_PACK_OFFSET(HkPackBodyWorldChildHeader, timingReserved, 92);

#pragma pack(push, 1)
typedef struct HkPackBodyWorldContainerHeader {
    HkPackObjectId containerStableId;
    uint64_t programSemanticFingerprint;
    uint32_t mode;
    uint32_t randomDiscipline;
    uint32_t memoryScope;
    uint32_t retriggerPolicy;
    uint32_t resetPolicy;
    uint32_t sequenceEndPolicy;
    uint32_t queueCycle;
    uint32_t queueSplice;
    uint32_t queueCycleCount;
    uint8_t resumePosition;
    uint8_t reserved[3];
    uint32_t avoidLastN;
    uint32_t initialSequenceCursor;
    uint32_t discreteTiming;
    uint32_t continuousTiming;
    uint32_t continuousNormalization;
    uint32_t evaluationPeriodFrames;
    uint32_t envelopeFrames;
    uint32_t maxActiveRows;
    uint32_t maxLiveRows;
    uint32_t childCount;
    uint32_t curveCount;
    uint32_t discreteAxisCount;
    uint32_t discreteRowCount;
    uint32_t continuousAxisCount;
    uint32_t continuousRowCount;
    uint32_t transitionProgramIndex;
    uint32_t authoredStableOrder;
} HkPackBodyWorldContainerHeader;
#pragma pack(pop)
HK_PACK_SIZE(HkPackBodyWorldContainerHeader, 132);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, containerStableId, 0);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, programSemanticFingerprint, 16);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, mode, 24);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, randomDiscipline, 28);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, memoryScope, 32);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, retriggerPolicy, 36);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, resetPolicy, 40);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, sequenceEndPolicy, 44);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, queueCycle, 48);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, queueSplice, 52);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, queueCycleCount, 56);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, resumePosition, 60);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, reserved, 61);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, avoidLastN, 64);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, initialSequenceCursor, 68);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, discreteTiming, 72);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, continuousTiming, 76);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, continuousNormalization, 80);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, evaluationPeriodFrames, 84);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, envelopeFrames, 88);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, maxActiveRows, 92);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, maxLiveRows, 96);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, childCount, 100);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, curveCount, 104);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, discreteAxisCount, 108);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, discreteRowCount, 112);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, continuousAxisCount, 116);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, continuousRowCount, 120);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, transitionProgramIndex, 124);
HK_PACK_OFFSET(HkPackBodyWorldContainerHeader, authoredStableOrder, 128);

typedef struct HkPackBodyWorldContinuousCell {
    uint32_t kind;
    float lower;
    float upper;
    uint8_t lowerInclusive;
    uint8_t upperInclusive;
    uint16_t reserved;
    uint32_t leftCurveLocalId;
    uint32_t rightCurveLocalId;
} HkPackBodyWorldContinuousCell;
HK_PACK_SIZE(HkPackBodyWorldContinuousCell, 24);
HK_PACK_OFFSET(HkPackBodyWorldContinuousCell, kind, 0);
HK_PACK_OFFSET(HkPackBodyWorldContinuousCell, lower, 4);
HK_PACK_OFFSET(HkPackBodyWorldContinuousCell, upper, 8);
HK_PACK_OFFSET(HkPackBodyWorldContinuousCell, lowerInclusive, 12);
HK_PACK_OFFSET(HkPackBodyWorldContinuousCell, upperInclusive, 13);
HK_PACK_OFFSET(HkPackBodyWorldContinuousCell, reserved, 14);
HK_PACK_OFFSET(HkPackBodyWorldContinuousCell, leftCurveLocalId, 16);
HK_PACK_OFFSET(HkPackBodyWorldContinuousCell, rightCurveLocalId, 20);

typedef struct HkPackBodyWorldContinuousGridHeader {
    uint32_t stableRowOrdinal;
    uint32_t authoredStableOrder;
    uint32_t childStableOrdinal;
    uint32_t cellCount;
} HkPackBodyWorldContinuousGridHeader;
HK_PACK_SIZE(HkPackBodyWorldContinuousGridHeader, 16);
HK_PACK_OFFSET(HkPackBodyWorldContinuousGridHeader, stableRowOrdinal, 0);
HK_PACK_OFFSET(HkPackBodyWorldContinuousGridHeader, authoredStableOrder, 4);
HK_PACK_OFFSET(HkPackBodyWorldContinuousGridHeader, childStableOrdinal, 8);
HK_PACK_OFFSET(HkPackBodyWorldContinuousGridHeader, cellCount, 12);

typedef struct HkPackBodyWorldCurveHandle {
    float h1x;
    float h1y;
    float h2x;
    float h2y;
} HkPackBodyWorldCurveHandle;
HK_PACK_SIZE(HkPackBodyWorldCurveHandle, 16);
HK_PACK_OFFSET(HkPackBodyWorldCurveHandle, h1x, 0);
HK_PACK_OFFSET(HkPackBodyWorldCurveHandle, h1y, 4);
HK_PACK_OFFSET(HkPackBodyWorldCurveHandle, h2x, 8);
HK_PACK_OFFSET(HkPackBodyWorldCurveHandle, h2y, 12);

typedef struct HkPackBodyWorldCurveHeader {
    uint32_t curveLocalId;
    uint32_t pointCount;
} HkPackBodyWorldCurveHeader;
HK_PACK_SIZE(HkPackBodyWorldCurveHeader, 8);
HK_PACK_OFFSET(HkPackBodyWorldCurveHeader, curveLocalId, 0);
HK_PACK_OFFSET(HkPackBodyWorldCurveHeader, pointCount, 4);

typedef struct HkPackBodyWorldCurvePoint {
    float x;
    float y;
} HkPackBodyWorldCurvePoint;
HK_PACK_SIZE(HkPackBodyWorldCurvePoint, 8);
HK_PACK_OFFSET(HkPackBodyWorldCurvePoint, x, 0);
HK_PACK_OFFSET(HkPackBodyWorldCurvePoint, y, 4);

typedef struct HkPackBodyWorldDiscreteCell {
    uint32_t kind;
    int32_t exactValue;
} HkPackBodyWorldDiscreteCell;
HK_PACK_SIZE(HkPackBodyWorldDiscreteCell, 8);
HK_PACK_OFFSET(HkPackBodyWorldDiscreteCell, kind, 0);
HK_PACK_OFFSET(HkPackBodyWorldDiscreteCell, exactValue, 4);

typedef struct HkPackBodyWorldDiscreteGridHeader {
    uint32_t stableRowOrdinal;
    uint32_t authoredStableOrder;
    uint32_t childStableOrdinal;
    uint32_t cellCount;
} HkPackBodyWorldDiscreteGridHeader;
HK_PACK_SIZE(HkPackBodyWorldDiscreteGridHeader, 16);
HK_PACK_OFFSET(HkPackBodyWorldDiscreteGridHeader, stableRowOrdinal, 0);
HK_PACK_OFFSET(HkPackBodyWorldDiscreteGridHeader, authoredStableOrder, 4);
HK_PACK_OFFSET(HkPackBodyWorldDiscreteGridHeader, childStableOrdinal, 8);
HK_PACK_OFFSET(HkPackBodyWorldDiscreteGridHeader, cellCount, 12);

typedef struct HkPackBodyWorldFade {
    uint8_t active;
    uint8_t reserved[3];
    uint32_t durationFrames;
    uint32_t curveLocalId;
} HkPackBodyWorldFade;
HK_PACK_SIZE(HkPackBodyWorldFade, 12);
HK_PACK_OFFSET(HkPackBodyWorldFade, active, 0);
HK_PACK_OFFSET(HkPackBodyWorldFade, reserved, 1);
HK_PACK_OFFSET(HkPackBodyWorldFade, durationFrames, 4);
HK_PACK_OFFSET(HkPackBodyWorldFade, curveLocalId, 8);

typedef struct HkPackBodyWorldGridAxis {
    uint32_t stableAxisOrdinal;
    uint32_t authoredColumnOrder;
    HkPackObjectId variableId;
} HkPackBodyWorldGridAxis;
HK_PACK_SIZE(HkPackBodyWorldGridAxis, 24);
HK_PACK_OFFSET(HkPackBodyWorldGridAxis, stableAxisOrdinal, 0);
HK_PACK_OFFSET(HkPackBodyWorldGridAxis, authoredColumnOrder, 4);
HK_PACK_OFFSET(HkPackBodyWorldGridAxis, variableId, 8);

typedef struct HkPackBodyWorldInlineSound {
    HkPackBodyWorldTime startOffset;
    HkPackBodyWorldTime duration;
    uint32_t loopCount;
    uint8_t loopRegionPresent;
    uint8_t reserved[3];
    HkPackBodyWorldTime loopStart;
    HkPackBodyWorldTime loopEnd;
} HkPackBodyWorldInlineSound;
HK_PACK_SIZE(HkPackBodyWorldInlineSound, 72);
HK_PACK_OFFSET(HkPackBodyWorldInlineSound, startOffset, 0);
HK_PACK_OFFSET(HkPackBodyWorldInlineSound, duration, 16);
HK_PACK_OFFSET(HkPackBodyWorldInlineSound, loopCount, 32);
HK_PACK_OFFSET(HkPackBodyWorldInlineSound, loopRegionPresent, 36);
HK_PACK_OFFSET(HkPackBodyWorldInlineSound, reserved, 37);
HK_PACK_OFFSET(HkPackBodyWorldInlineSound, loopStart, 40);
HK_PACK_OFFSET(HkPackBodyWorldInlineSound, loopEnd, 56);

typedef struct HkPackBodyWorldLayerAncestor {
    HkPackObjectId layerStableId;
    uint64_t layerProgramSemanticFingerprint;
} HkPackBodyWorldLayerAncestor;
HK_PACK_SIZE(HkPackBodyWorldLayerAncestor, 24);
HK_PACK_OFFSET(HkPackBodyWorldLayerAncestor, layerStableId, 0);
HK_PACK_OFFSET(HkPackBodyWorldLayerAncestor, layerProgramSemanticFingerprint, 16);

typedef struct HkPackBodyWorldMarker {
    HkPackObjectId stableMarkerId;
    uint32_t markerId;
    uint32_t markerKind;
    uint64_t position;
} HkPackBodyWorldMarker;
HK_PACK_SIZE(HkPackBodyWorldMarker, 32);
HK_PACK_OFFSET(HkPackBodyWorldMarker, stableMarkerId, 0);
HK_PACK_OFFSET(HkPackBodyWorldMarker, markerId, 16);
HK_PACK_OFFSET(HkPackBodyWorldMarker, markerKind, 20);
HK_PACK_OFFSET(HkPackBodyWorldMarker, position, 24);

typedef struct HkPackBodyWorldOccurrenceHeader {
    uint32_t parentKind;
    uint32_t stableOccurrenceOrdinal;
    HkPackObjectId parentStableId;
    uint64_t parentProgramSemanticFingerprint;
    HkPackBodyWorldPlayableRef childPlayable;
    uint64_t occurrenceSemanticFingerprint;
} HkPackBodyWorldOccurrenceHeader;
HK_PACK_SIZE(HkPackBodyWorldOccurrenceHeader, 104);
HK_PACK_OFFSET(HkPackBodyWorldOccurrenceHeader, parentKind, 0);
HK_PACK_OFFSET(HkPackBodyWorldOccurrenceHeader, stableOccurrenceOrdinal, 4);
HK_PACK_OFFSET(HkPackBodyWorldOccurrenceHeader, parentStableId, 8);
HK_PACK_OFFSET(HkPackBodyWorldOccurrenceHeader, parentProgramSemanticFingerprint, 24);
HK_PACK_OFFSET(HkPackBodyWorldOccurrenceHeader, childPlayable, 32);
HK_PACK_OFFSET(HkPackBodyWorldOccurrenceHeader, occurrenceSemanticFingerprint, 96);

typedef struct HkPackBodyWorldPathStep {
    HkPackBodyWorldPlayableRef playable;
    uint32_t stableChildOrdinal;
    uint32_t reserved;
    HkPackObjectId trackId;
    HkPackObjectId clipId;
    uint64_t clipGuidHigh;
    uint64_t clipGuidLow;
} HkPackBodyWorldPathStep;
HK_PACK_SIZE(HkPackBodyWorldPathStep, 120);
HK_PACK_OFFSET(HkPackBodyWorldPathStep, playable, 0);
HK_PACK_OFFSET(HkPackBodyWorldPathStep, stableChildOrdinal, 64);
HK_PACK_OFFSET(HkPackBodyWorldPathStep, reserved, 68);
HK_PACK_OFFSET(HkPackBodyWorldPathStep, trackId, 72);
HK_PACK_OFFSET(HkPackBodyWorldPathStep, clipId, 88);
HK_PACK_OFFSET(HkPackBodyWorldPathStep, clipGuidHigh, 104);
HK_PACK_OFFSET(HkPackBodyWorldPathStep, clipGuidLow, 112);

typedef struct HkPackBodyWorldPlacement {
    uint32_t stablePlacementOrdinal;
    uint32_t stableOccurrenceOrdinal;
    HkPackBodyWorldPlayableRef content;
    uint64_t authoredStart;
    uint64_t authoredDuration;
    uint64_t authoredFadeIn;
    uint64_t authoredFadeOut;
    HkPackObjectId clipId;
    uint64_t clipGuidHigh;
    uint64_t clipGuidLow;
} HkPackBodyWorldPlacement;
HK_PACK_SIZE(HkPackBodyWorldPlacement, 136);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, stablePlacementOrdinal, 0);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, stableOccurrenceOrdinal, 4);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, content, 8);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, authoredStart, 72);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, authoredDuration, 80);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, authoredFadeIn, 88);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, authoredFadeOut, 96);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, clipId, 104);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, clipGuidHigh, 120);
HK_PACK_OFFSET(HkPackBodyWorldPlacement, clipGuidLow, 128);

typedef struct HkPackBodyWorldPrefix {
    uint32_t schema;
    uint32_t compiler;
    uint32_t selectionAlgorithm;
    uint32_t gridAlgorithm;
    uint32_t transitionAlgorithm;
    uint32_t publicationProtocol;
    uint64_t runtimeSemanticFingerprint;
    uint64_t capabilityMask;
    uint32_t variableCount;
    uint32_t playablePropertyProgramCount;
    uint32_t playableOccurrencePropertyCount;
    uint32_t playablePropertySendCount;
    uint32_t reservedChildCount1;
    uint32_t reservedChildCount2;
    uint32_t playablePropertyLayerAncestorRefCount;
    uint32_t playablePropertyEffectNodeCount;
    uint32_t playablePropertyEffectParamCount;
    uint32_t playablePropertyEffectEdgeCount;
    uint32_t trackScheduleCount;
    uint32_t segmentScheduleCount;
    uint32_t scheduleTrackRefCount;
    uint32_t schedulePlacementCount;
    uint32_t scheduleMarkerCount;
    uint32_t containerCount;
    uint32_t transitionProgramCount;
    uint32_t maxContainerDepth;
    uint32_t maxChildrenPerContainer;
    uint32_t maxAvoidLastN;
    uint32_t maxDiscreteAxes;
    uint32_t maxDiscreteRows;
    uint32_t maxDiscreteTrieNodes;
    uint32_t maxContinuousAxes;
    uint32_t maxContinuousRows;
    uint32_t maxContinuousCandidates;
    uint32_t maxContinuousActiveRows;
    uint32_t maxContinuousLiveRows;
    uint32_t maxTransitionRules;
    uint32_t maxCurvePoints;
    uint32_t maxKnownScheduleLeaves;
    uint32_t maxTrackSchedulePrograms;
    uint32_t maxSegmentSchedulePrograms;
    uint32_t maxScheduleTrackRefsPerSegment;
    uint32_t maxScheduleTrackRefsTotal;
    uint32_t maxSchedulePlacementsPerTrack;
    uint32_t maxSchedulePlacementsTotal;
    uint32_t maxScheduleMarkersPerSegment;
    uint32_t maxScheduleMarkersTotal;
    uint32_t maxTrackSegmentScheduleDepth;
} HkPackBodyWorldPrefix;
HK_PACK_SIZE(HkPackBodyWorldPrefix, 200);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, schema, 0);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, compiler, 4);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, selectionAlgorithm, 8);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, gridAlgorithm, 12);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, transitionAlgorithm, 16);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, publicationProtocol, 20);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, runtimeSemanticFingerprint, 24);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, capabilityMask, 32);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, variableCount, 40);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, playablePropertyProgramCount, 44);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, playableOccurrencePropertyCount, 48);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, playablePropertySendCount, 52);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, reservedChildCount1, 56);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, reservedChildCount2, 60);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, playablePropertyLayerAncestorRefCount, 64);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, playablePropertyEffectNodeCount, 68);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, playablePropertyEffectParamCount, 72);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, playablePropertyEffectEdgeCount, 76);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, trackScheduleCount, 80);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, segmentScheduleCount, 84);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, scheduleTrackRefCount, 88);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, schedulePlacementCount, 92);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, scheduleMarkerCount, 96);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, containerCount, 100);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, transitionProgramCount, 104);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxContainerDepth, 108);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxChildrenPerContainer, 112);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxAvoidLastN, 116);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxDiscreteAxes, 120);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxDiscreteRows, 124);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxDiscreteTrieNodes, 128);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxContinuousAxes, 132);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxContinuousRows, 136);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxContinuousCandidates, 140);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxContinuousActiveRows, 144);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxContinuousLiveRows, 148);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxTransitionRules, 152);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxCurvePoints, 156);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxKnownScheduleLeaves, 160);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxTrackSchedulePrograms, 164);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxSegmentSchedulePrograms, 168);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxScheduleTrackRefsPerSegment, 172);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxScheduleTrackRefsTotal, 176);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxSchedulePlacementsPerTrack, 180);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxSchedulePlacementsTotal, 184);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxScheduleMarkersPerSegment, 188);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxScheduleMarkersTotal, 192);
HK_PACK_OFFSET(HkPackBodyWorldPrefix, maxTrackSegmentScheduleDepth, 196);

typedef struct HkPackBodyWorldProjectId {
    HkPackObjectId value;
} HkPackBodyWorldProjectId;
HK_PACK_SIZE(HkPackBodyWorldProjectId, 16);
HK_PACK_OFFSET(HkPackBodyWorldProjectId, value, 0);

typedef struct HkPackBodyWorldPropertyHeader {
    uint32_t contentPropertyKind;
    uint32_t authoredStableOrder;
    HkPackObjectId stableId;
    uint64_t programSemanticFingerprint;
    uint64_t contentRevision;
    HkPackObjectId routeStableId;
    uint64_t compatibilityFingerprint;
    uint32_t organizationLayerAncestorCount;
    uint32_t reserved;
} HkPackBodyWorldPropertyHeader;
HK_PACK_SIZE(HkPackBodyWorldPropertyHeader, 72);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, contentPropertyKind, 0);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, authoredStableOrder, 4);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, stableId, 8);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, programSemanticFingerprint, 24);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, contentRevision, 32);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, routeStableId, 40);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, compatibilityFingerprint, 56);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, organizationLayerAncestorCount, 64);
HK_PACK_OFFSET(HkPackBodyWorldPropertyHeader, reserved, 68);

typedef struct HkPackBodyWorldScheduleKey {
    uint32_t kind;
    uint32_t reserved;
    HkPackObjectId stableId;
    uint64_t scheduleSemanticFingerprint;
} HkPackBodyWorldScheduleKey;
HK_PACK_SIZE(HkPackBodyWorldScheduleKey, 32);
HK_PACK_OFFSET(HkPackBodyWorldScheduleKey, kind, 0);
HK_PACK_OFFSET(HkPackBodyWorldScheduleKey, reserved, 4);
HK_PACK_OFFSET(HkPackBodyWorldScheduleKey, stableId, 8);
HK_PACK_OFFSET(HkPackBodyWorldScheduleKey, scheduleSemanticFingerprint, 24);

typedef struct HkPackBodyWorldSegmentEntryTimeline {
    HkPackObjectId stableMarkerId;
    uint64_t position;
    uint64_t contentStart;
    uint64_t contentEnd;
    uint64_t nominalStart;
    uint64_t nominalEnd;
} HkPackBodyWorldSegmentEntryTimeline;
HK_PACK_SIZE(HkPackBodyWorldSegmentEntryTimeline, 56);
HK_PACK_OFFSET(HkPackBodyWorldSegmentEntryTimeline, stableMarkerId, 0);
HK_PACK_OFFSET(HkPackBodyWorldSegmentEntryTimeline, position, 16);
HK_PACK_OFFSET(HkPackBodyWorldSegmentEntryTimeline, contentStart, 24);
HK_PACK_OFFSET(HkPackBodyWorldSegmentEntryTimeline, contentEnd, 32);
HK_PACK_OFFSET(HkPackBodyWorldSegmentEntryTimeline, nominalStart, 40);
HK_PACK_OFFSET(HkPackBodyWorldSegmentEntryTimeline, nominalEnd, 48);

typedef struct HkPackBodyWorldSegmentHeader {
    HkPackBodyWorldPlayableRef rootDefinition;
    uint64_t scheduleSemanticFingerprint;
    uint32_t timeKind;
    uint32_t startPointKind;
    uint32_t startMarkerId;
    uint32_t reserved;
    uint64_t microsPerQuarter;
    uint32_t meterNumerator;
    uint32_t meterDenominator;
    uint32_t trackRefCount;
    uint32_t markerCount;
} HkPackBodyWorldSegmentHeader;
HK_PACK_SIZE(HkPackBodyWorldSegmentHeader, 112);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, rootDefinition, 0);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, scheduleSemanticFingerprint, 64);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, timeKind, 72);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, startPointKind, 76);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, startMarkerId, 80);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, reserved, 84);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, microsPerQuarter, 88);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, meterNumerator, 96);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, meterDenominator, 100);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, trackRefCount, 104);
HK_PACK_OFFSET(HkPackBodyWorldSegmentHeader, markerCount, 108);

typedef struct HkPackBodyWorldSignalCounts {
    uint32_t signalRelationCount;
    uint32_t signalReceiverCount;
} HkPackBodyWorldSignalCounts;
HK_PACK_SIZE(HkPackBodyWorldSignalCounts, 8);
HK_PACK_OFFSET(HkPackBodyWorldSignalCounts, signalRelationCount, 0);
HK_PACK_OFFSET(HkPackBodyWorldSignalCounts, signalReceiverCount, 4);

typedef struct HkPackBodyWorldSignalEndpoint {
    uint32_t kind;
    uint32_t reserved;
    HkPackObjectId stableId;
    uint64_t expectedPropertyFingerprint;
    uint64_t expectedProgramFingerprint;
} HkPackBodyWorldSignalEndpoint;
HK_PACK_SIZE(HkPackBodyWorldSignalEndpoint, 40);
HK_PACK_OFFSET(HkPackBodyWorldSignalEndpoint, kind, 0);
HK_PACK_OFFSET(HkPackBodyWorldSignalEndpoint, reserved, 4);
HK_PACK_OFFSET(HkPackBodyWorldSignalEndpoint, stableId, 8);
HK_PACK_OFFSET(HkPackBodyWorldSignalEndpoint, expectedPropertyFingerprint, 24);
HK_PACK_OFFSET(HkPackBodyWorldSignalEndpoint, expectedProgramFingerprint, 32);

typedef struct HkPackBodyWorldSignalReceiver {
    HkPackBodyWorldSignalEndpoint target;
    uint32_t algorithm;
    float attackMs;
    float releaseMs;
    uint32_t reserved;
} HkPackBodyWorldSignalReceiver;
HK_PACK_SIZE(HkPackBodyWorldSignalReceiver, 56);
HK_PACK_OFFSET(HkPackBodyWorldSignalReceiver, target, 0);
HK_PACK_OFFSET(HkPackBodyWorldSignalReceiver, algorithm, 40);
HK_PACK_OFFSET(HkPackBodyWorldSignalReceiver, attackMs, 44);
HK_PACK_OFFSET(HkPackBodyWorldSignalReceiver, releaseMs, 48);
HK_PACK_OFFSET(HkPackBodyWorldSignalReceiver, reserved, 52);

typedef struct HkPackBodyWorldSignalRelation {
    HkPackObjectId stableId;
    HkPackBodyWorldSignalEndpoint source;
    HkPackBodyWorldSignalEndpoint target;
    uint32_t consumer;
    float sendGainDb;
} HkPackBodyWorldSignalRelation;
HK_PACK_SIZE(HkPackBodyWorldSignalRelation, 104);
HK_PACK_OFFSET(HkPackBodyWorldSignalRelation, stableId, 0);
HK_PACK_OFFSET(HkPackBodyWorldSignalRelation, source, 16);
HK_PACK_OFFSET(HkPackBodyWorldSignalRelation, target, 56);
HK_PACK_OFFSET(HkPackBodyWorldSignalRelation, consumer, 96);
HK_PACK_OFFSET(HkPackBodyWorldSignalRelation, sendGainDb, 100);

typedef struct HkPackBodyWorldStructuralAncestorHeader {
    uint32_t contentPropertyKind;
    uint32_t reserved;
    HkPackObjectId stableId;
    uint64_t programSemanticFingerprint;
} HkPackBodyWorldStructuralAncestorHeader;
HK_PACK_SIZE(HkPackBodyWorldStructuralAncestorHeader, 32);
HK_PACK_OFFSET(HkPackBodyWorldStructuralAncestorHeader, contentPropertyKind, 0);
HK_PACK_OFFSET(HkPackBodyWorldStructuralAncestorHeader, reserved, 4);
HK_PACK_OFFSET(HkPackBodyWorldStructuralAncestorHeader, stableId, 8);
HK_PACK_OFFSET(HkPackBodyWorldStructuralAncestorHeader, programSemanticFingerprint, 24);

typedef struct HkPackBodyWorldStructuralCount {
    uint32_t value;
} HkPackBodyWorldStructuralCount;
HK_PACK_SIZE(HkPackBodyWorldStructuralCount, 4);
HK_PACK_OFFSET(HkPackBodyWorldStructuralCount, value, 0);

typedef struct HkPackBodyWorldStructuralHeader {
    uint32_t structuralAncestorCount;
    uint32_t reserved;
} HkPackBodyWorldStructuralHeader;
HK_PACK_SIZE(HkPackBodyWorldStructuralHeader, 8);
HK_PACK_OFFSET(HkPackBodyWorldStructuralHeader, structuralAncestorCount, 0);
HK_PACK_OFFSET(HkPackBodyWorldStructuralHeader, reserved, 4);

typedef struct HkPackBodyWorldTrackHeader {
    HkPackBodyWorldPlayableRef rootDefinition;
    uint64_t scheduleSemanticFingerprint;
    uint32_t placementCount;
    uint32_t reserved;
} HkPackBodyWorldTrackHeader;
HK_PACK_SIZE(HkPackBodyWorldTrackHeader, 80);
HK_PACK_OFFSET(HkPackBodyWorldTrackHeader, rootDefinition, 0);
HK_PACK_OFFSET(HkPackBodyWorldTrackHeader, scheduleSemanticFingerprint, 64);
HK_PACK_OFFSET(HkPackBodyWorldTrackHeader, placementCount, 72);
HK_PACK_OFFSET(HkPackBodyWorldTrackHeader, reserved, 76);

typedef struct HkPackBodyWorldTrackRef {
    HkPackBodyWorldPlayableRef trackDefinition;
    uint64_t trackScheduleSemanticFingerprint;
    uint32_t stableTrackOrdinal;
    uint32_t reserved;
} HkPackBodyWorldTrackRef;
HK_PACK_SIZE(HkPackBodyWorldTrackRef, 80);
HK_PACK_OFFSET(HkPackBodyWorldTrackRef, trackDefinition, 0);
HK_PACK_OFFSET(HkPackBodyWorldTrackRef, trackScheduleSemanticFingerprint, 64);
HK_PACK_OFFSET(HkPackBodyWorldTrackRef, stableTrackOrdinal, 72);
HK_PACK_OFFSET(HkPackBodyWorldTrackRef, reserved, 76);

typedef struct HkPackBodyWorldTransitionHeader {
    HkPackObjectId ownerContainerStableId;
    uint32_t ruleCount;
    uint32_t defaultRuleIndex;
    uint32_t downwardPathStepCount;
    uint32_t maxContainerDescent;
    uint32_t maxOwnerAscent;
    uint32_t watchFallback;
} HkPackBodyWorldTransitionHeader;
HK_PACK_SIZE(HkPackBodyWorldTransitionHeader, 40);
HK_PACK_OFFSET(HkPackBodyWorldTransitionHeader, ownerContainerStableId, 0);
HK_PACK_OFFSET(HkPackBodyWorldTransitionHeader, ruleCount, 16);
HK_PACK_OFFSET(HkPackBodyWorldTransitionHeader, defaultRuleIndex, 20);
HK_PACK_OFFSET(HkPackBodyWorldTransitionHeader, downwardPathStepCount, 24);
HK_PACK_OFFSET(HkPackBodyWorldTransitionHeader, maxContainerDescent, 28);
HK_PACK_OFFSET(HkPackBodyWorldTransitionHeader, maxOwnerAscent, 32);
HK_PACK_OFFSET(HkPackBodyWorldTransitionHeader, watchFallback, 36);

typedef struct HkPackBodyWorldTransitionRule {
    uint32_t stableRuleOrdinal;
    uint8_t sourceAny;
    uint8_t targetAny;
    uint8_t bridgePresent;
    uint8_t reserved;
    HkPackBodyWorldPlayableRef source;
    HkPackBodyWorldPlayableRef target;
    uint32_t sourceExit;
    uint32_t targetEntry;
    HkPackBodyWorldPlayableRef bridge;
    HkPackBodyWorldFade sourceFade;
    HkPackBodyWorldFade bridgeFadeIn;
    HkPackBodyWorldFade bridgeFadeOut;
    HkPackBodyWorldFade targetFade;
    uint64_t requiredClockFingerprint;
    HkPackObjectId requiredMarkerStableId;
    uint64_t samePositionCompatibilityFingerprint;
    uint32_t authoredStableOrder;
    uint32_t reserved2;
} HkPackBodyWorldTransitionRule;
HK_PACK_SIZE(HkPackBodyWorldTransitionRule, 296);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, stableRuleOrdinal, 0);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, sourceAny, 4);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, targetAny, 5);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, bridgePresent, 6);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, reserved, 7);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, source, 8);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, target, 72);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, sourceExit, 136);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, targetEntry, 140);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, bridge, 144);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, sourceFade, 208);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, bridgeFadeIn, 220);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, bridgeFadeOut, 232);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, targetFade, 244);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, requiredClockFingerprint, 256);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, requiredMarkerStableId, 264);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, samePositionCompatibilityFingerprint, 280);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, authoredStableOrder, 288);
HK_PACK_OFFSET(HkPackBodyWorldTransitionRule, reserved2, 292);

typedef struct HkPackBodyWorldVariableHeader {
    HkPackObjectId variableId;
    uint32_t kind;
    uint32_t scope;
    float rangeMin;
    float rangeMax;
    float defaultValue;
    uint32_t reserved;
} HkPackBodyWorldVariableHeader;
HK_PACK_SIZE(HkPackBodyWorldVariableHeader, 40);
HK_PACK_OFFSET(HkPackBodyWorldVariableHeader, variableId, 0);
HK_PACK_OFFSET(HkPackBodyWorldVariableHeader, kind, 16);
HK_PACK_OFFSET(HkPackBodyWorldVariableHeader, scope, 20);
HK_PACK_OFFSET(HkPackBodyWorldVariableHeader, rangeMin, 24);
HK_PACK_OFFSET(HkPackBodyWorldVariableHeader, rangeMax, 28);
HK_PACK_OFFSET(HkPackBodyWorldVariableHeader, defaultValue, 32);
HK_PACK_OFFSET(HkPackBodyWorldVariableHeader, reserved, 36);

#endif
