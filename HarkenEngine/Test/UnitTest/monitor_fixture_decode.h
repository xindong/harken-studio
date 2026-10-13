#ifndef HARKEN_MONITOR_FIXTURE_DECODE_H
#define HARKEN_MONITOR_FIXTURE_DECODE_H

#include <harken/Harken.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VnMonitorDecodedRecord {
    VnMonitorRecordHeader header;
    uint32_t numericReadable;
    uint32_t reserved0;
    const uint8_t* payload;
    uint64_t payloadBytes;
    const uint8_t* skippedBytes;
    uint64_t skippedBytesCount;
} VnMonitorDecodedRecord;

typedef struct VnMonitorDecodeResult {
    VnResult status;
    uint32_t recordsWritten;
    uint64_t bytesConsumed;
    uint64_t nextRecordBytes;
} VnMonitorDecodeResult;

uint32_t vn_monitor_crc32c(const void* data, size_t length);

VnMonitorDecodeResult vn_monitor_decode_records(
    const uint8_t* input,
    uint64_t inputBytes,
    VnMonitorDecodedRecord* outRecords,
    uint32_t outCapacity,
    uint8_t* skipBuffer,
    uint64_t skipCapacity);

#ifdef __cplusplus
}
#endif

#endif
