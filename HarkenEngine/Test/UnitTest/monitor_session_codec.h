#ifndef HARKEN_MONITOR_SESSION_CODEC_H
#define HARKEN_MONITOR_SESSION_CODEC_H

#include "monitor_fixture_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Packed on-disk Session file header. Not sizeof(VnMonitorSessionFileHeader). */
#define VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES 64u

typedef struct VnMonitorSessionCommitView {
    uint64_t chunkSequence;
    uint32_t kind;
    uint32_t flags;
    uint32_t visible;
    uint32_t reserved0;
    const uint8_t* payload;
    uint64_t payloadBytes;
    const uint8_t* rawBytes;
    uint64_t rawByteCount;
} VnMonitorSessionCommitView;

typedef struct VnMonitorSessionScanResult {
    VnResult status;
    uint32_t commitsWritten;
    uint32_t droppedRequired;
    uint32_t droppedOptional;
    uint64_t bytesConsumed;
    uint64_t fileMagic;
    uint64_t capabilityFingerprint;
    uint64_t engineMonitorInstanceId;
} VnMonitorSessionScanResult;

VnMonitorSessionScanResult vn_monitor_scan_session(
    const uint8_t* input,
    uint64_t inputBytes,
    VnMonitorSessionCommitView* outCommits,
    uint32_t outCapacity,
    VnMonitorDecodedRecord* recordScratch,
    uint32_t recordScratchCapacity,
    uint8_t* skipBuffer,
    uint64_t skipCapacity);

uint32_t vn_monitor_compare_sessions(
    const uint8_t* left,
    uint64_t leftBytes,
    const uint8_t* right,
    uint64_t rightBytes);

#ifdef __cplusplus
}
#endif

#endif
