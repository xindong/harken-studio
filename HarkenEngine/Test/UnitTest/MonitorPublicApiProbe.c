#include <harken/Harken.h>
#include <harken/HarkenDevelopment.h>

#include "MonitorFixtureDecode.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HARKEN_MONITOR_FIXTURE_DIR
#define HARKEN_MONITOR_FIXTURE_DIR "."
#endif

#if defined(__cplusplus)
#error "MonitorPublicApiProbe.c must be compiled as C"
#endif

static int Fail(const char* kind, const char* detail) {
    fprintf(stderr, "f0-probe %s: %s\n", kind, detail);
    return 1;
}

static uint8_t* ReadAll(const char* path, uint64_t* outBytes) {
    FILE* file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t* bytes = (uint8_t*)malloc((size_t)size);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    size_t readCount = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    if (readCount != (size_t)size) {
        free(bytes);
        return NULL;
    }
    *outBytes = (uint64_t)size;
    return bytes;
}

int main(void) {
    char recordPath[1024];
    if (snprintf(recordPath, sizeof(recordPath), "%s/%s", HARKEN_MONITOR_FIXTURE_DIR,
                 "legal_event.bin") < 0)
        return Fail("missing-sample", "record path");

    uint64_t recordBytes = 0;
    uint8_t* record = ReadAll(recordPath, &recordBytes);
    if (record == NULL)
        return Fail("missing-sample", recordPath);
    if (recordBytes < 8) {
        free(record);
        return Fail("missing-sample", "record too small");
    }
    uint64_t declaredCount = (uint64_t)record[0] | ((uint64_t)record[1] << 8) |
                             ((uint64_t)record[2] << 16) | ((uint64_t)record[3] << 24) |
                             ((uint64_t)record[4] << 32) | ((uint64_t)record[5] << 40) |
                             ((uint64_t)record[6] << 48) | ((uint64_t)record[7] << 56);
    if (declaredCount != 1) {
        free(record);
        return Fail("decode-error", "record count prefix");
    }
    const uint8_t* packed = record + 8;
    uint64_t packedBytes = recordBytes - 8;
    VnMonitorDecodedRecord decoded[4];
    uint8_t skip[4096];
    memset(decoded, 0, sizeof(decoded));
    memset(skip, 0, sizeof(skip));
    VnMonitorDecodeResult records = vn_monitor_decode_records(
        packed, packedBytes, decoded, 4, skip, sizeof(skip));
    if (records.status != VN_OK || records.recordsWritten != 1u) {
        char detail[128];
        snprintf(detail, sizeof(detail), "record status=%d written=%u count=%llu",
                 (int)records.status, records.recordsWritten,
                 (unsigned long long)declaredCount);
        free(record);
        return Fail("decode-error", detail);
    }
    free(record);
    return 0;
}
