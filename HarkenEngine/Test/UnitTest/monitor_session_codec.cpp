#include "monitor_session_codec.h"

#include <cstring>
#include <vector>

namespace {

uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

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

bool AddOverflow(uint64_t a, uint64_t b, uint64_t* out) {
    if (a > UINT64_MAX - b)
        return true;
    *out = a + b;
    return false;
}

/* Packed 32 minus the trailing headerCrc32c field. */
uint32_t HeaderCrc(const uint8_t* packed32) {
    return vn_monitor_crc32c(packed32, 28);
}

bool ChunkRequired(uint32_t flags) {
    const uint32_t required = flags & VN_MONITOR_CHUNK_REQUIRED;
    const uint32_t optional = flags & VN_MONITOR_CHUNK_OPTIONAL;
    return required != 0 && optional == 0;
}

bool ChunkOptional(uint32_t flags) {
    const uint32_t required = flags & VN_MONITOR_CHUNK_REQUIRED;
    const uint32_t optional = flags & VN_MONITOR_CHUNK_OPTIONAL;
    return optional != 0 && required == 0;
}

bool KnownChunkKind(uint32_t kind) {
    return kind >= VN_MONITOR_CHUNK_SESSION_HEADER &&
           kind <= VN_MONITOR_CHUNK_RECOVERY_INDEX;
}

bool RecordStreamKind(uint32_t kind) {
    return kind == VN_MONITOR_CHUNK_EVENTS || kind == VN_MONITOR_CHUNK_STATE ||
           kind == VN_MONITOR_CHUNK_TIME_SERIES ||
           kind == VN_MONITOR_CHUNK_OPTIONAL_AUDIO ||
           kind == VN_MONITOR_CHUNK_DROP_SUMMARIES;
}

VnResult DecodeCommitRecords(const uint8_t* payload,
                             uint64_t payloadBytes,
                             VnMonitorDecodedRecord* scratch,
                             uint32_t scratchCapacity,
                             uint8_t* skipBuffer,
                             uint64_t skipCapacity) {
    if (payloadBytes == 0)
        return VN_OK;
    auto decoded = vn_monitor_decode_records(payload, payloadBytes, scratch,
                                             scratchCapacity, skipBuffer,
                                             skipCapacity);
    if (decoded.status != VN_OK)
        return decoded.status;
    if (decoded.bytesConsumed != payloadBytes)
        return VN_MONITOR_INCOMPATIBLE;
    return VN_OK;
}

} // namespace

VnMonitorSessionScanResult vn_monitor_scan_session(
    const uint8_t* input,
    uint64_t inputBytes,
    VnMonitorSessionCommitView* outCommits,
    uint32_t outCapacity,
    VnMonitorDecodedRecord* recordScratch,
    uint32_t recordScratchCapacity,
    uint8_t* skipBuffer,
    uint64_t skipCapacity) {
    VnMonitorSessionScanResult result{};
    result.status = VN_OK;
    if (input == nullptr && inputBytes != 0) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    if (outCapacity > 0 && outCommits == nullptr) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    if (inputBytes > VN_MONITOR_HARD_SESSION_FILE_BYTES) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    if (inputBytes < VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }

    const uint32_t major = ReadU32(input);
    const uint32_t minor = ReadU32(input + 4);
    if (major != VN_MONITOR_SESSION_FORMAT_MAJOR ||
        minor != VN_MONITOR_SESSION_FORMAT_MINOR) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    const uint64_t magic = ReadU64(input + 12);
    if (magic != VN_MONITOR_SESSION_FILE_MAGIC) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    const uint64_t headerBytes = ReadU64(input + 20);
    if (headerBytes != VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    uint8_t headerCopy[VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES];
    std::memcpy(headerCopy, input, VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES);
    headerCopy[60] = 0;
    headerCopy[61] = 0;
    headerCopy[62] = 0;
    headerCopy[63] = 0;
    const uint32_t headerCrc = ReadU32(input + 60);
    if (vn_monitor_crc32c(headerCopy, VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES) !=
        headerCrc) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    result.fileMagic = magic;
    result.engineMonitorInstanceId = ReadU64(input + 36);
    result.capabilityFingerprint = ReadU64(input + 52);

    uint64_t offset = VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES;
    uint64_t lastSequence = 0;
    bool haveSequence = false;
    while (offset < inputBytes) {
        const uint64_t remaining = inputBytes - offset;
        if (remaining < VN_MONITOR_SESSION_CHUNK_HEADER_BYTES) {
            result.bytesConsumed = offset;
            return result;
        }
        const uint8_t* chunk = input + offset;
        const uint32_t kind = ReadU16(chunk);
        const uint32_t flags = ReadU16(chunk + 2);
        const uint32_t version = ReadU16(chunk + 4);
        const uint32_t packedHeaderBytes = ReadU16(chunk + 6);
        const uint64_t payloadBytes = ReadU64(chunk + 8);
        const uint64_t chunkSequence = ReadU64(chunk + 16);
        const uint32_t payloadCrc = ReadU32(chunk + 24);
        const uint32_t headerCrc32 = ReadU32(chunk + 28);
        if (packedHeaderBytes != VN_MONITOR_SESSION_CHUNK_HEADER_BYTES ||
            version != 1u ||
            HeaderCrc(chunk) != headerCrc32) {
            result.bytesConsumed = offset;
            return result;
        }
        if (payloadBytes > VN_MONITOR_HARD_CHUNK_PAYLOAD_BYTES) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.bytesConsumed = offset;
            return result;
        }
        uint64_t afterPayload = 0;
        if (AddOverflow(offset, VN_MONITOR_SESSION_CHUNK_HEADER_BYTES, &afterPayload) ||
            AddOverflow(afterPayload, payloadBytes, &afterPayload)) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.bytesConsumed = offset;
            return result;
        }
        uint64_t commitEnd = 0;
        if (AddOverflow(afterPayload, VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES,
                        &commitEnd)) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.bytesConsumed = offset;
            return result;
        }
        if (commitEnd > inputBytes) {
            result.bytesConsumed = offset;
            return result;
        }
        const uint8_t* payload = chunk + VN_MONITOR_SESSION_CHUNK_HEADER_BYTES;
        const uint8_t* footer = payload + payloadBytes;
        const uint64_t footerMagic = ReadU64(footer);
        const uint64_t footerSequence = ReadU64(footer + 8);
        const uint64_t footerPayload = ReadU64(footer + 16);
        if (footerMagic != VN_MONITOR_SESSION_COMMIT_MAGIC ||
            footerSequence != chunkSequence ||
            footerPayload != payloadBytes) {
            result.bytesConsumed = offset;
            return result;
        }
        if (haveSequence) {
            uint64_t expected = 0;
            if (AddOverflow(lastSequence, 1ull, &expected) ||
                chunkSequence != expected) {
                result.status = VN_MONITOR_INCOMPATIBLE;
                result.bytesConsumed = offset;
                return result;
            }
        }
        if (vn_monitor_crc32c(payload, static_cast<size_t>(payloadBytes)) !=
            payloadCrc) {
            if (ChunkRequired(flags))
                result.droppedRequired += 1;
            else if (ChunkOptional(flags))
                result.droppedOptional += 1;
            haveSequence = true;
            lastSequence = chunkSequence;
            offset = commitEnd;
            continue;
        }
        const bool known = KnownChunkKind(kind);
        const bool required = ChunkRequired(flags);
        if (!required && !ChunkOptional(flags)) {
            result.status = VN_INVALID_ARG;
            result.bytesConsumed = offset;
            return result;
        }
        if (!known && required) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.bytesConsumed = offset;
            return result;
        }
        if (known && required && RecordStreamKind(kind) &&
            recordScratchCapacity != 0) {
            VnResult decoded = DecodeCommitRecords(payload, payloadBytes, recordScratch,
                                                   recordScratchCapacity, skipBuffer,
                                                   skipCapacity);
            if (decoded == VN_MONITOR_BUFFER_TOO_SMALL) {
                result.status = VN_MONITOR_BUFFER_TOO_SMALL;
                result.bytesConsumed = offset;
                return result;
            }
            if (decoded != VN_OK) {
                result.droppedRequired += 1;
                haveSequence = true;
                lastSequence = chunkSequence;
                offset = commitEnd;
                continue;
            }
        }
        if (outCapacity == 0) {
            result.commitsWritten += 1;
            haveSequence = true;
            lastSequence = chunkSequence;
            offset = commitEnd;
            result.bytesConsumed = offset;
            continue;
        }
        if (result.commitsWritten >= outCapacity) {
            result.status = VN_MONITOR_BUFFER_TOO_SMALL;
            result.bytesConsumed = offset;
            return result;
        }
        VnMonitorSessionCommitView view{};
        view.chunkSequence = chunkSequence;
        view.kind = kind;
        view.flags = flags;
        view.visible = 1;
        view.payload = payload;
        view.payloadBytes = payloadBytes;
        view.rawBytes = chunk;
        view.rawByteCount = commitEnd - offset;
        outCommits[result.commitsWritten] = view;
        result.commitsWritten += 1;
        haveSequence = true;
        lastSequence = chunkSequence;
        offset = commitEnd;
        result.bytesConsumed = offset;
    }
    result.bytesConsumed = offset;
    return result;
}

VnMonitorSessionScanResult ScanWithGrownScratch(
    const uint8_t* bytes,
    uint64_t size,
    VnMonitorSessionCommitView* views,
    uint32_t cap) {
    uint32_t scratchCap = 16;
    uint64_t skipCap = 4096;
    const uint32_t scratchHard = 1u << 20;
    const uint64_t skipHard = VN_MONITOR_HARD_CHUNK_PAYLOAD_BYTES;
    VnMonitorSessionScanResult result{};
    for (;;) {
        std::vector<VnMonitorDecodedRecord> scratch(scratchCap);
        std::vector<uint8_t> skip(static_cast<size_t>(skipCap));
        result = vn_monitor_scan_session(bytes, size, views, cap, scratch.data(),
                                         scratchCap, skip.data(), skipCap);
        if (result.status != VN_MONITOR_BUFFER_TOO_SMALL)
            return result;
        const bool scratchMaxed = scratchCap >= scratchHard;
        const bool skipMaxed = skipCap >= skipHard;
        if (scratchMaxed && skipMaxed)
            return result;
        if (!scratchMaxed) {
            if (scratchCap > scratchHard / 2u)
                scratchCap = scratchHard;
            else
                scratchCap *= 2;
        }
        if (!skipMaxed) {
            if (skipCap > skipHard / 2ull)
                skipCap = skipHard;
            else
                skipCap *= 2;
        }
    }
}

uint32_t vn_monitor_compare_sessions(const uint8_t* left,
                                     uint64_t leftBytes,
                                     const uint8_t* right,
                                     uint64_t rightBytes) {
    auto leftCount = vn_monitor_scan_session(left, leftBytes, nullptr, 0, nullptr, 0,
                                             nullptr, 0);
    auto rightCount = vn_monitor_scan_session(right, rightBytes, nullptr, 0, nullptr, 0,
                                              nullptr, 0);
    if (leftCount.status != VN_OK || rightCount.status != VN_OK)
        return VN_MONITOR_COMPARE_MISSING_DATA;
    const uint32_t leftCap = leftCount.commitsWritten == 0 ? 1u : leftCount.commitsWritten;
    const uint32_t rightCap = rightCount.commitsWritten == 0 ? 1u : rightCount.commitsWritten;
    std::vector<VnMonitorSessionCommitView> leftStore(leftCap);
    std::vector<VnMonitorSessionCommitView> rightStore(rightCap);
    auto ls = ScanWithGrownScratch(left, leftBytes, leftStore.data(), leftCap);
    auto rs = ScanWithGrownScratch(right, rightBytes, rightStore.data(), rightCap);
    if (ls.status != VN_OK || rs.status != VN_OK)
        return VN_MONITOR_COMPARE_MISSING_DATA;
    const bool leftLeftover = ls.bytesConsumed < leftBytes;
    const bool rightLeftover = rs.bytesConsumed < rightBytes;
    if (leftLeftover || rightLeftover)
        return VN_MONITOR_COMPARE_MISSING_DATA;
    if (ls.commitsWritten == 0 || rs.commitsWritten == 0)
        return VN_MONITOR_COMPARE_MISSING_DATA;
    auto unknownOptional = [](const VnMonitorSessionCommitView* views, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const bool known = views[i].kind >= VN_MONITOR_CHUNK_SESSION_HEADER &&
                               views[i].kind <= VN_MONITOR_CHUNK_RECOVERY_INDEX;
            const bool optional = (views[i].flags & VN_MONITOR_CHUNK_OPTIONAL) != 0 &&
                                  (views[i].flags & VN_MONITOR_CHUNK_REQUIRED) == 0;
            if (optional && !known)
                return true;
        }
        return false;
    };
    const bool partialUnknown = unknownOptional(leftStore.data(), ls.commitsWritten) ||
                                unknownOptional(rightStore.data(), rs.commitsWritten);
    const bool partialDropped = ls.droppedRequired != 0 || rs.droppedRequired != 0 ||
                                ls.droppedOptional != 0 || rs.droppedOptional != 0;
    if (partialUnknown || partialDropped)
        return VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE;
    if (ls.capabilityFingerprint != rs.capabilityFingerprint ||
        ls.engineMonitorInstanceId != rs.engineMonitorInstanceId)
        return VN_MONITOR_COMPARE_INCOMPARABLE;
    uint32_t leftVisible = 0;
    uint32_t rightVisible = 0;
    for (uint32_t i = 0; i < ls.commitsWritten; ++i)
        leftVisible += leftStore[i].visible;
    for (uint32_t i = 0; i < rs.commitsWritten; ++i)
        rightVisible += rightStore[i].visible;
    if (leftVisible == 0 || rightVisible == 0)
        return VN_MONITOR_COMPARE_MISSING_DATA;
    if (leftVisible != ls.commitsWritten || rightVisible != rs.commitsWritten)
        return VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE;
    return VN_MONITOR_COMPARE_COMPARABLE;
}
