#ifndef HARKEN_MONITOR_REMOTE_CODEC_H
#define HARKEN_MONITOR_REMOTE_CODEC_H

#include <harken/Harken.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VN_MONITOR_REMOTE_HANDSHAKE_PACKED_BYTES 120u
#define VN_MONITOR_REMOTE_FRAME_COUNTER_PACKED_BYTES 16u

typedef struct VnMonitorRemotePrincipal {
    uint32_t profile;
    uint32_t reserved0;
    uint64_t localAclSid;
    uint8_t lanMtlsSpkiSha256[VN_MONITOR_REMOTE_SPKI_SHA256_BYTES];
} VnMonitorRemotePrincipal;

typedef struct VnMonitorRemoteDecodeResult {
    VnResult status;
    uint32_t grantedPermissionMask;
    uint64_t bytesConsumed;
    uint64_t messageCounter;
    VnMonitorRemoteHandshake handshake;
    VnMonitorRemotePrincipal principal;
} VnMonitorRemoteDecodeResult;

VnMonitorRemoteDecodeResult vn_monitor_decode_remote_handshake(
    const uint8_t* input,
    uint64_t inputBytes,
    uint32_t expectedProfile,
    uint64_t expectedSchemaFingerprint,
    uint64_t expectedBuildFingerprint,
    uint64_t expectedProgramFingerprint,
    uint64_t expectedCapabilityFingerprint,
    uint32_t requiredPermissionMask,
    const uint8_t* principalBytes,
    uint64_t principalByteCount);

VnResult vn_monitor_decode_remote_counter(
    const uint8_t* input,
    uint64_t inputBytes,
    uint64_t previousCounter,
    uint64_t* outCounter);

#ifdef __cplusplus
}
#endif

#endif
