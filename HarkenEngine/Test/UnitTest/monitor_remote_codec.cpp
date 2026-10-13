#include "monitor_remote_codec.h"

#include <cstring>

namespace {

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64(const uint8_t* p) {
    return static_cast<uint64_t>(ReadU32(p)) |
           (static_cast<uint64_t>(ReadU32(p + 4)) << 32);
}

bool KnownProfile(uint32_t profile) {
    return profile == VN_MONITOR_SECURITY_DISABLED ||
           profile == VN_MONITOR_SECURITY_LOCAL_ACL ||
           profile == VN_MONITOR_SECURITY_LAN_MTLS;
}

constexpr uint32_t kKnownPermissionMask =
    VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_OBSERVE_EVENTS |
    VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_CAPTURE_CONTROL |
    VN_MONITOR_PERM_RECORD | VN_MONITOR_PERM_RAW_AUDIO | VN_MONITOR_PERM_SYMBOLS;

} // namespace

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
    uint64_t principalByteCount) {
    VnMonitorRemoteDecodeResult result{};
    result.status = VN_OK;
    if (input == nullptr && inputBytes != 0) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    if (principalBytes == nullptr && principalByteCount != 0) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    if (inputBytes != VN_MONITOR_REMOTE_HANDSHAKE_PACKED_BYTES) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    const uint32_t structSize = ReadU32(input);
    if (structSize != VN_MONITOR_REMOTE_HANDSHAKE_PACKED_BYTES) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    const uint32_t profile = ReadU32(input + 4);
    if (!KnownProfile(profile)) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    if (profile != expectedProfile) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    const uint32_t reserved0 = ReadU32(input + 52);
    if (reserved0 != 0) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    const uint64_t schema = ReadU64(input + 16);
    const uint64_t build = ReadU64(input + 24);
    const uint64_t program = ReadU64(input + 32);
    const uint64_t capability = ReadU64(input + 40);
    if (schema != expectedSchemaFingerprint || build != expectedBuildFingerprint ||
        program != expectedProgramFingerprint ||
        capability != expectedCapabilityFingerprint) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    const uint32_t requested = ReadU32(input + 48);
    if ((requested & ~kKnownPermissionMask) != 0) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    if ((requiredPermissionMask & ~kKnownPermissionMask) != 0) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    if ((requiredPermissionMask & requested) != requiredPermissionMask) {
        result.status = VN_MONITOR_PERMISSION_DENIED;
        return result;
    }
    if (profile == VN_MONITOR_SECURITY_LOCAL_ACL) {
        if (principalByteCount != 8)
            result.status = VN_MONITOR_INCOMPATIBLE;
        else
            result.principal.localAclSid = ReadU64(principalBytes);
    } else if (profile == VN_MONITOR_SECURITY_LAN_MTLS) {
        if (principalByteCount != VN_MONITOR_REMOTE_SPKI_SHA256_BYTES)
            result.status = VN_MONITOR_INCOMPATIBLE;
        else
            std::memcpy(result.principal.lanMtlsSpkiSha256, principalBytes,
                        VN_MONITOR_REMOTE_SPKI_SHA256_BYTES);
    } else if (principalByteCount != 0) {
        result.status = VN_INVALID_ARG;
    }
    if (result.status != VN_OK)
        return result;
    result.handshake.structSize = sizeof(VnMonitorRemoteHandshake);
    result.handshake.securityProfile = profile;
    result.handshake.protocolMajor = ReadU32(input + 8);
    result.handshake.protocolMinor = ReadU32(input + 12);
    result.handshake.schemaFingerprint = schema;
    result.handshake.buildFingerprint = build;
    result.handshake.programSemanticFingerprint = program;
    result.handshake.capabilityFingerprint = capability;
    result.handshake.requestedPermissionMask = requested;
    result.handshake.reserved0 = 0;
    for (int i = 0; i < 4; ++i) {
        result.handshake.clientNonce[i] = ReadU64(input + 56 + static_cast<size_t>(i) * 8);
        result.handshake.serverNonce[i] = ReadU64(input + 88 + static_cast<size_t>(i) * 8);
    }
    result.principal.profile = profile;
    result.grantedPermissionMask = requested;
    result.bytesConsumed = VN_MONITOR_REMOTE_HANDSHAKE_PACKED_BYTES;
    result.messageCounter = VN_MONITOR_REMOTE_MESSAGE_COUNTER_START;
    return result;
}

VnResult vn_monitor_decode_remote_counter(
    const uint8_t* input,
    uint64_t inputBytes,
    uint64_t previousCounter,
    uint64_t* outCounter) {
    if (outCounter == nullptr)
        return VN_INVALID_ARG;
    *outCounter = 0;
    if (input == nullptr && inputBytes != 0)
        return VN_INVALID_ARG;
    if (inputBytes != VN_MONITOR_REMOTE_FRAME_COUNTER_PACKED_BYTES)
        return VN_MONITOR_INCOMPATIBLE;
    const uint32_t structSize = ReadU32(input);
    if (structSize != VN_MONITOR_REMOTE_FRAME_COUNTER_PACKED_BYTES)
        return VN_MONITOR_INCOMPATIBLE;
    if (ReadU32(input + 4) != 0)
        return VN_INVALID_ARG;
    const uint64_t value = ReadU64(input + 8);
    if (previousCounter == 0) {
        if (value != VN_MONITOR_REMOTE_MESSAGE_COUNTER_START)
            return VN_MONITOR_INCOMPATIBLE;
    } else if (previousCounter == UINT64_MAX) {
        return VN_MONITOR_INCOMPATIBLE;
    } else if (value != previousCounter + 1ull) {
        return VN_MONITOR_INCOMPATIBLE;
    }
    *outCounter = value;
    return VN_OK;
}
