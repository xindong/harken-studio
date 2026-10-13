#include <harken/Harken.h>

#include "monitor_fixture_decode.h"
#include "monitor_session_codec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HARKEN_MONITOR_FIXTURE_DIR
#define HARKEN_MONITOR_FIXTURE_DIR "."
#endif
#ifndef HARKEN_MONITOR_SESSION_FIXTURE_DIR
#define HARKEN_MONITOR_SESSION_FIXTURE_DIR "."
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
    char sessionPath[1024];
    if (snprintf(recordPath, sizeof(recordPath), "%s/%s", HARKEN_MONITOR_FIXTURE_DIR,
                 "legal_event.bin") < 0)
        return Fail("missing-sample", "record path");
    if (snprintf(sessionPath, sizeof(sessionPath), "%s/%s", HARKEN_MONITOR_SESSION_FIXTURE_DIR,
                 "legal.bin") < 0)
        return Fail("missing-sample", "session path");

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

    uint64_t sessionBytes = 0;
    uint8_t* session = ReadAll(sessionPath, &sessionBytes);
    if (session == NULL)
        return Fail("missing-sample", sessionPath);
    VnMonitorSessionCommitView commits[8];
    VnMonitorDecodedRecord scratch[8];
    uint8_t sessionSkip[2048];
    memset(commits, 0, sizeof(commits));
    memset(scratch, 0, sizeof(scratch));
    memset(sessionSkip, 0, sizeof(sessionSkip));
    VnMonitorSessionScanResult scanned = vn_monitor_scan_session(
        session, sessionBytes, commits, 8, scratch, 8, sessionSkip, sizeof(sessionSkip));
    free(session);
    if (scanned.status != VN_OK || scanned.commitsWritten == 0)
        return Fail("decode-error", "session");
    return 0;
}
