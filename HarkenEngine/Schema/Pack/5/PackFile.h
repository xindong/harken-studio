#ifndef HARKEN_PACK_FILE_H
#define HARKEN_PACK_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "PackVersion.h"

#ifdef __cplusplus
#define HK_PACK_STATIC_ASSERT(condition, message) static_assert(condition, message)
#define HK_PACK_TABLE inline constexpr
#else
#define HK_PACK_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#define HK_PACK_TABLE static const
#endif

#define HK_PACK_SIZE(type, bytes) HK_PACK_STATIC_ASSERT(sizeof(type) == (bytes), #type)
#define HK_PACK_OFFSET(type, field, offset) HK_PACK_STATIC_ASSERT(offsetof(type, field) == (offset), #type "." #field)

#define HK_PACK_STREAM_ONCE 0u
#define HK_PACK_STREAM_EACH 1u
#define HK_PACK_STREAM_EACH_LESS_ONE 2u
#define HK_PACK_STREAM_WHEN 3u

typedef struct HkPackStreamStep {
    const char* part;
    uint32_t partBytes;
    const struct HkPackStreamStep* nested;
    uint32_t nestedSteps;
    uint32_t repeat;
    const char* counter;
    uint32_t counterOffset;
    uint32_t counterBytes;
    uint64_t whenValue;
} HkPackStreamStep;

#define HK_PACK_STREAM_FIELD_BYTES(type, field) ((uint32_t)sizeof(((type*)0)->field))
#define HK_PACK_STREAM_COUNTER(repeat, counter, field, value) \
    (repeat), #counter, (uint32_t)offsetof(counter, field), HK_PACK_STREAM_FIELD_BYTES(counter, field), (value)
#define HK_PACK_STREAM_RECORD(type) {#type, (uint32_t)sizeof(type), NULL, 0u, HK_PACK_STREAM_ONCE, NULL, 0u, 0u, 0u}
#define HK_PACK_STREAM_RECORD_EACH(type, counter, field) \
    {#type, (uint32_t)sizeof(type), NULL, 0u, HK_PACK_STREAM_COUNTER(HK_PACK_STREAM_EACH, counter, field, 0u)}
#define HK_PACK_STREAM_RECORD_EACH_LESS_ONE(type, counter, field) \
    {#type, (uint32_t)sizeof(type), NULL, 0u, HK_PACK_STREAM_COUNTER(HK_PACK_STREAM_EACH_LESS_ONE, counter, field, 0u)}
#define HK_PACK_STREAM_RECORD_WHEN(type, counter, field, value) \
    {#type, (uint32_t)sizeof(type), NULL, 0u, HK_PACK_STREAM_COUNTER(HK_PACK_STREAM_WHEN, counter, field, value)}
#define HK_PACK_STREAM_STEPS(stream) ((uint32_t)(sizeof(stream) / sizeof((stream)[0])))
#define HK_PACK_STREAM_NESTED(stream) {#stream, 0u, stream, HK_PACK_STREAM_STEPS(stream), HK_PACK_STREAM_ONCE, NULL, 0u, 0u, 0u}
#define HK_PACK_STREAM_NESTED_EACH(stream, counter, field) \
    {#stream, 0u, stream, HK_PACK_STREAM_STEPS(stream), HK_PACK_STREAM_COUNTER(HK_PACK_STREAM_EACH, counter, field, 0u)}
#define HK_PACK_STREAM_NESTED_WHEN(stream, counter, field, value) \
    {#stream, 0u, stream, HK_PACK_STREAM_STEPS(stream), HK_PACK_STREAM_COUNTER(HK_PACK_STREAM_WHEN, counter, field, value)}

typedef struct HkPackObjectId {
    uint8_t bytes[16];
} HkPackObjectId;
HK_PACK_SIZE(HkPackObjectId, 16);

#define HK_PACK_BLOB_ALIGN UINT64_C(64)
#define HK_PACK_BUS_DEFINITION_DOWNMIX_COEFFICIENT_COUNT UINT64_C(64)
#define HK_PACK_PAYLOAD_ALIGN UINT64_C(8)
#define HK_PACK_SHIPPING_BODY_VINTAGE UINT64_C(18)
#define HK_PACK_ENTRY_SCHEDULING_BODY_VINTAGE UINT64_C(19)
#define HK_PACK_SIGNAL_RELATIONS_BODY_VINTAGE UINT64_C(20)
#define HK_PACK_VOICE_RECIPE_MAX_PARAMS UINT64_C(64)
#define HK_PACK_EFFECT_PRESET_REFERENCE_TYPE UINT64_C(4294967295)

#define HK_PACK_ENTRY_KIND_MEDIA 1u
#define HK_PACK_ENTRY_KIND_BUS 2u
#define HK_PACK_ENTRY_KIND_BUS_SEND 3u
#define HK_PACK_ENTRY_KIND_ATTENUATION 4u
#define HK_PACK_ENTRY_KIND_RETIRED_DISCRETE_VARIABLE 5u
#define HK_PACK_ENTRY_KIND_RETIRED_METER_WIRING 6u
#define HK_PACK_ENTRY_KIND_BINDING_DEFINITION 7u
#define HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT 8u
#define HK_PACK_ENTRY_KIND_CONTAINER 9u
#define HK_PACK_ENTRY_KIND_RETIRED_ACTION 10u
#define HK_PACK_ENTRY_KIND_VOICE_RECIPE 11u
#define HK_PACK_ENTRY_KIND_BUS_EFFECT 12u
#define HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE 13u
#define HK_PACK_ENTRY_KIND_RETIRED_INIT_ROUTING_CATALOG 20u
#define HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION 21u
#define HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK 22u
#define HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE 23u
#define HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT 24u
#define HK_PACK_ENTRY_KIND_RETIRED_GAMEPLAY_CATALOG 25u
#define HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY 26u
#define HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS 27u
#define HK_PACK_ENTRY_KIND_RETIRED_VARIABLE_NAME_CATALOG 28u
#define HK_PACK_ENTRY_KIND_EFFECT_PRESET 29u

#define HK_PACK_MEDIA_VINTAGE 2u
#define HK_PACK_BUS_VINTAGE 6u
#define HK_PACK_BUS_SEND_VINTAGE 2u
#define HK_PACK_ATTENUATION_VINTAGE 3u
#define HK_PACK_BINDING_DEFINITION_VINTAGE 2u
#define HK_PACK_BINDING_ATTACHMENT_VINTAGE 2u
#define HK_PACK_CONTAINER_VINTAGE 2u
#define HK_PACK_VOICE_RECIPE_VINTAGE 5u
#define HK_PACK_BUS_EFFECT_VINTAGE 1u
#define HK_PACK_EFFECT_DYNAMIC_ROUTE_VINTAGE 1u
#define HK_PACK_CANONICAL_DEFINITION_PARTITION_VINTAGE 2u
#define HK_PACK_PACK_REQUIREMENT_BLOCK_VINTAGE 2u
#define HK_PACK_PACK_DEPENDENCY_EDGE_VINTAGE 2u
#define HK_PACK_BUS_DEFINITION_SNAPSHOT_VINTAGE 2u
#define HK_PACK_VARIABLE_POLICY_OVERLAY_VINTAGE 1u
#define HK_PACK_VARIABLE_REQUIREMENTS_VINTAGE 1u
#define HK_PACK_EFFECT_PRESET_VINTAGE 1u

#define HK_PACK_KIND_RESIDENT 0u
#define HK_PACK_KIND_CONTENT 1u
#define HK_PACK_KIND_INIT 2u
#define HK_PACK_KIND_CANONICAL_CONTENT 3u

#define HK_PACK_DEPENDENCY_EDGE_KIND_INIT 1u
#define HK_PACK_DEPENDENCY_EDGE_KIND_SHARED 2u
#define HK_PACK_DEPENDENCY_EDGE_KIND_CONTENT 3u

#define HK_PACK_CANONICAL_PARTITION_HEADER_MAGIC "VNDEFP02"
#define HK_PACK_MAGIC "VSPK"

typedef struct HkPackCanonicalPartitionHeader {
    uint8_t magic[8];
    uint32_t headerBytes;
    uint32_t bodyVintage;
    uint64_t bodyBytes;
    uint64_t bodyChecksum;
    uint64_t packStableId;
    uint64_t partitionRevision;
    uint64_t ownedDefinitionCount;
    uint64_t busContentGeneration;
    uint64_t publishedBusFingerprint;
    uint64_t compileTargetProfileFingerprint;
    uint64_t headerChecksum;
} HkPackCanonicalPartitionHeader;
HK_PACK_SIZE(HkPackCanonicalPartitionHeader, 88);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, magic, 0);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, headerBytes, 8);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, bodyVintage, 12);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, bodyBytes, 16);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, bodyChecksum, 24);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, packStableId, 32);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, partitionRevision, 40);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, ownedDefinitionCount, 48);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, busContentGeneration, 56);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, publishedBusFingerprint, 64);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, compileTargetProfileFingerprint, 72);
HK_PACK_OFFSET(HkPackCanonicalPartitionHeader, headerChecksum, 80);

typedef struct HkPackDependencyEdge {
    uint64_t prerequisitePackStableId;
    uint32_t edgeKind;
    uint32_t required01;
    uint64_t expectedPrerequisiteContentRevision;
    uint64_t expectedBusContentGeneration;
    uint64_t expectedPublishedBusFingerprint;
    uint64_t expectedCompileTargetProfileFingerprint;
} HkPackDependencyEdge;
HK_PACK_SIZE(HkPackDependencyEdge, 48);
HK_PACK_OFFSET(HkPackDependencyEdge, prerequisitePackStableId, 0);
HK_PACK_OFFSET(HkPackDependencyEdge, edgeKind, 8);
HK_PACK_OFFSET(HkPackDependencyEdge, required01, 12);
HK_PACK_OFFSET(HkPackDependencyEdge, expectedPrerequisiteContentRevision, 16);
HK_PACK_OFFSET(HkPackDependencyEdge, expectedBusContentGeneration, 24);
HK_PACK_OFFSET(HkPackDependencyEdge, expectedPublishedBusFingerprint, 32);
HK_PACK_OFFSET(HkPackDependencyEdge, expectedCompileTargetProfileFingerprint, 40);

typedef struct HkPackDirectoryEntry {
    uint32_t kind;
    uint32_t vintage;
    HkPackObjectId id;
    uint64_t offset;
    uint64_t size;
    uint64_t payloadChecksum;
    uint64_t relationOrdinal;
} HkPackDirectoryEntry;
HK_PACK_SIZE(HkPackDirectoryEntry, 56);
HK_PACK_OFFSET(HkPackDirectoryEntry, kind, 0);
HK_PACK_OFFSET(HkPackDirectoryEntry, vintage, 4);
HK_PACK_OFFSET(HkPackDirectoryEntry, id, 8);
HK_PACK_OFFSET(HkPackDirectoryEntry, offset, 24);
HK_PACK_OFFSET(HkPackDirectoryEntry, size, 32);
HK_PACK_OFFSET(HkPackDirectoryEntry, payloadChecksum, 40);
HK_PACK_OFFSET(HkPackDirectoryEntry, relationOrdinal, 48);

#pragma pack(push, 1)
typedef struct HkPackFileHeader {
    uint8_t magic[4];
    uint32_t formatVersion;
    uint32_t headerBytes;
    uint64_t fileBytes;
    uint32_t packKind;
    uint32_t entryCount;
    uint64_t directoryOffset;
    uint64_t directoryChecksum;
} HkPackFileHeader;
#pragma pack(pop)
HK_PACK_SIZE(HkPackFileHeader, 44);
HK_PACK_OFFSET(HkPackFileHeader, magic, 0);
HK_PACK_OFFSET(HkPackFileHeader, formatVersion, 4);
HK_PACK_OFFSET(HkPackFileHeader, headerBytes, 8);
HK_PACK_OFFSET(HkPackFileHeader, fileBytes, 12);
HK_PACK_OFFSET(HkPackFileHeader, packKind, 20);
HK_PACK_OFFSET(HkPackFileHeader, entryCount, 24);
HK_PACK_OFFSET(HkPackFileHeader, directoryOffset, 28);
HK_PACK_OFFSET(HkPackFileHeader, directoryChecksum, 36);

typedef struct HkPackFilePrefix {
    uint8_t magic[4];
    uint32_t formatVersion;
    uint32_t headerBytes;
} HkPackFilePrefix;
HK_PACK_SIZE(HkPackFilePrefix, 12);
HK_PACK_OFFSET(HkPackFilePrefix, magic, 0);
HK_PACK_OFFSET(HkPackFilePrefix, formatVersion, 4);
HK_PACK_OFFSET(HkPackFilePrefix, headerBytes, 8);

#pragma pack(push, 1)
typedef struct HkPackMediaFileHeader {
    uint8_t magic[4];
    uint32_t formatVersion;
    uint32_t headerBytes;
    uint64_t fileBytes;
    uint32_t entryCount;
    uint32_t tocOffset;
    uint64_t tocChecksum;
} HkPackMediaFileHeader;
#pragma pack(pop)
HK_PACK_SIZE(HkPackMediaFileHeader, 36);
HK_PACK_OFFSET(HkPackMediaFileHeader, magic, 0);
HK_PACK_OFFSET(HkPackMediaFileHeader, formatVersion, 4);
HK_PACK_OFFSET(HkPackMediaFileHeader, headerBytes, 8);
HK_PACK_OFFSET(HkPackMediaFileHeader, fileBytes, 12);
HK_PACK_OFFSET(HkPackMediaFileHeader, entryCount, 20);
HK_PACK_OFFSET(HkPackMediaFileHeader, tocOffset, 24);
HK_PACK_OFFSET(HkPackMediaFileHeader, tocChecksum, 28);

typedef struct HkPackRequirementBlock {
    uint32_t schemaVersion;
    uint32_t storageModelVersion;
    uint64_t packStableId;
    uint64_t packContentRevision;
    uint64_t definitionPartitionRevision;
    uint64_t resourceClosureRevision;
    uint64_t semanticFingerprint;
    uint64_t lineageFingerprint;
    uint32_t ownedDefinitionCount;
    uint32_t ownedResourceCount;
    uint64_t installedDefinitionBytes;
    uint64_t installedResourceBytes;
    uint64_t replacementPeakBytes;
    uint64_t decodeStagingBytes;
    uint32_t requiredCapabilityMask;
    uint32_t reserved0;
    uint64_t payloadChecksumCoverage;
    uint64_t busContentGeneration;
    uint64_t publishedBusFingerprint;
    uint64_t compileTargetProfileFingerprint;
} HkPackRequirementBlock;
HK_PACK_SIZE(HkPackRequirementBlock, 136);
HK_PACK_OFFSET(HkPackRequirementBlock, schemaVersion, 0);
HK_PACK_OFFSET(HkPackRequirementBlock, storageModelVersion, 4);
HK_PACK_OFFSET(HkPackRequirementBlock, packStableId, 8);
HK_PACK_OFFSET(HkPackRequirementBlock, packContentRevision, 16);
HK_PACK_OFFSET(HkPackRequirementBlock, definitionPartitionRevision, 24);
HK_PACK_OFFSET(HkPackRequirementBlock, resourceClosureRevision, 32);
HK_PACK_OFFSET(HkPackRequirementBlock, semanticFingerprint, 40);
HK_PACK_OFFSET(HkPackRequirementBlock, lineageFingerprint, 48);
HK_PACK_OFFSET(HkPackRequirementBlock, ownedDefinitionCount, 56);
HK_PACK_OFFSET(HkPackRequirementBlock, ownedResourceCount, 60);
HK_PACK_OFFSET(HkPackRequirementBlock, installedDefinitionBytes, 64);
HK_PACK_OFFSET(HkPackRequirementBlock, installedResourceBytes, 72);
HK_PACK_OFFSET(HkPackRequirementBlock, replacementPeakBytes, 80);
HK_PACK_OFFSET(HkPackRequirementBlock, decodeStagingBytes, 88);
HK_PACK_OFFSET(HkPackRequirementBlock, requiredCapabilityMask, 96);
HK_PACK_OFFSET(HkPackRequirementBlock, reserved0, 100);
HK_PACK_OFFSET(HkPackRequirementBlock, payloadChecksumCoverage, 104);
HK_PACK_OFFSET(HkPackRequirementBlock, busContentGeneration, 112);
HK_PACK_OFFSET(HkPackRequirementBlock, publishedBusFingerprint, 120);
HK_PACK_OFFSET(HkPackRequirementBlock, compileTargetProfileFingerprint, 128);

#endif
