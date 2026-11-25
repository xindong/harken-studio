#ifndef HARKEN_PACK_VARIABLE_H
#define HARKEN_PACK_VARIABLE_H

#include "PackFile.h"

#define HK_PACK_VARIABLE_POLICY_HEADER_FORMAT 2u
#define HK_PACK_VARIABLE_REQUIREMENT_IDENTITY_CLASS 1u
#define HK_PACK_VARIABLE_REQUIREMENTS_HEADER_FORMAT 1u

typedef struct HkPackVariableAnchor {
    int32_t anchor;
} HkPackVariableAnchor;
HK_PACK_SIZE(HkPackVariableAnchor, 4);
HK_PACK_OFFSET(HkPackVariableAnchor, anchor, 0);

typedef struct HkPackVariableAnchorCount {
    uint32_t count;
} HkPackVariableAnchorCount;
HK_PACK_SIZE(HkPackVariableAnchorCount, 4);
HK_PACK_OFFSET(HkPackVariableAnchorCount, count, 0);

typedef struct HkPackVariableLabel {
    HkPackObjectId labelId;
    int32_t anchor;
    uint32_t reserved;
} HkPackVariableLabel;
HK_PACK_SIZE(HkPackVariableLabel, 24);
HK_PACK_OFFSET(HkPackVariableLabel, labelId, 0);
HK_PACK_OFFSET(HkPackVariableLabel, anchor, 16);
HK_PACK_OFFSET(HkPackVariableLabel, reserved, 20);

typedef struct HkPackVariableLabelHeader {
    uint64_t labelMappingFingerprint;
    uint32_t labelCount;
    uint32_t reserved;
} HkPackVariableLabelHeader;
HK_PACK_SIZE(HkPackVariableLabelHeader, 16);
HK_PACK_OFFSET(HkPackVariableLabelHeader, labelMappingFingerprint, 0);
HK_PACK_OFFSET(HkPackVariableLabelHeader, labelCount, 8);
HK_PACK_OFFSET(HkPackVariableLabelHeader, reserved, 12);

typedef struct HkPackVariablePolicyHeader {
    uint32_t rowCount;
    uint32_t format;
    uint64_t reserved;
} HkPackVariablePolicyHeader;
HK_PACK_SIZE(HkPackVariablePolicyHeader, 16);
HK_PACK_OFFSET(HkPackVariablePolicyHeader, rowCount, 0);
HK_PACK_OFFSET(HkPackVariablePolicyHeader, format, 4);
HK_PACK_OFFSET(HkPackVariablePolicyHeader, reserved, 8);

typedef struct HkPackVariablePolicyRow {
    HkPackObjectId variableId;
    uint8_t writers;
    uint8_t reserved[7];
    uint64_t executionFingerprint;
} HkPackVariablePolicyRow;
HK_PACK_SIZE(HkPackVariablePolicyRow, 32);
HK_PACK_OFFSET(HkPackVariablePolicyRow, variableId, 0);
HK_PACK_OFFSET(HkPackVariablePolicyRow, writers, 16);
HK_PACK_OFFSET(HkPackVariablePolicyRow, reserved, 17);
HK_PACK_OFFSET(HkPackVariablePolicyRow, executionFingerprint, 24);

typedef struct HkPackVariableRequirement {
    uint8_t identityClass;
    uint8_t reserved[7];
    HkPackObjectId stableId;
    uint64_t expectedExecutionFingerprint;
    uint64_t expectedLabelMappingFingerprint;
} HkPackVariableRequirement;
HK_PACK_SIZE(HkPackVariableRequirement, 40);
HK_PACK_OFFSET(HkPackVariableRequirement, identityClass, 0);
HK_PACK_OFFSET(HkPackVariableRequirement, reserved, 1);
HK_PACK_OFFSET(HkPackVariableRequirement, stableId, 8);
HK_PACK_OFFSET(HkPackVariableRequirement, expectedExecutionFingerprint, 24);
HK_PACK_OFFSET(HkPackVariableRequirement, expectedLabelMappingFingerprint, 32);

typedef struct HkPackVariableRequirementsHeader {
    uint32_t rowCount;
    uint32_t format;
    uint64_t reserved;
} HkPackVariableRequirementsHeader;
HK_PACK_SIZE(HkPackVariableRequirementsHeader, 16);
HK_PACK_OFFSET(HkPackVariableRequirementsHeader, rowCount, 0);
HK_PACK_OFFSET(HkPackVariableRequirementsHeader, format, 4);
HK_PACK_OFFSET(HkPackVariableRequirementsHeader, reserved, 8);

HK_PACK_TABLE HkPackStreamStep hkPackStreamVariableAnchorSection[] = {
    HK_PACK_STREAM_RECORD(HkPackVariableAnchorCount),
    HK_PACK_STREAM_RECORD_EACH(HkPackVariableAnchor, HkPackVariableAnchorCount, count),
};

HK_PACK_TABLE HkPackStreamStep hkPackStreamVariableLabelSection[] = {
    HK_PACK_STREAM_RECORD(HkPackVariableLabelHeader),
    HK_PACK_STREAM_RECORD_EACH(HkPackVariableLabel, HkPackVariableLabelHeader, labelCount),
};

HK_PACK_TABLE HkPackStreamStep hkPackStreamVariablePolicyPayload[] = {
    HK_PACK_STREAM_RECORD(HkPackVariablePolicyHeader),
    HK_PACK_STREAM_RECORD_EACH(HkPackVariablePolicyRow, HkPackVariablePolicyHeader, rowCount),
    HK_PACK_STREAM_NESTED_EACH(hkPackStreamVariableAnchorSection, HkPackVariablePolicyHeader, rowCount),
    HK_PACK_STREAM_NESTED_EACH(hkPackStreamVariableLabelSection, HkPackVariablePolicyHeader, rowCount),
};

HK_PACK_TABLE HkPackStreamStep hkPackStreamVariableRequirementsPayload[] = {
    HK_PACK_STREAM_RECORD(HkPackVariableRequirementsHeader),
    HK_PACK_STREAM_RECORD_EACH(HkPackVariableRequirement, HkPackVariableRequirementsHeader, rowCount),
};

#endif
