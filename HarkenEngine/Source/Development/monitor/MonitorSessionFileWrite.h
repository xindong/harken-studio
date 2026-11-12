#ifndef HARKEN_API_MONITOR_SESSION_FILE_WRITE_H
#define HARKEN_API_MONITOR_SESSION_FILE_WRITE_H

#include "MonitorRecordPack.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace harken {
namespace monitor {

inline void WriteU16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

inline constexpr std::uint32_t kSessionFileHeaderPackedBytes = 64u;
inline constexpr std::uint32_t kTraceCorrelationPackedBytes = 64u;
inline constexpr std::uint32_t kTraceCorrelationPayloadVersion = 1u;

inline void PackTraceCorrelationPayload(std::uint8_t out[kTraceCorrelationPackedBytes],
                                        std::uint64_t engineMonitorInstanceId,
                                        std::uint64_t monitorSessionEpoch,
                                        std::uint64_t executeFrame,
                                        std::uint64_t logicCommitSequence,
                                        std::uint64_t actionInstanceHandle,
                                        std::uint64_t playableInstanceHandle,
                                        std::uint64_t traceCorrelationId) {
    std::memset(out, 0, kTraceCorrelationPackedBytes);
    WriteU32(out, kTraceCorrelationPayloadVersion);
    WriteU64(out + 8, engineMonitorInstanceId);
    WriteU64(out + 16, monitorSessionEpoch);
    WriteU64(out + 24, executeFrame);
    WriteU64(out + 32, logicCommitSequence);
    WriteU64(out + 40, actionInstanceHandle);
    WriteU64(out + 48, playableInstanceHandle);
    WriteU64(out + 56, traceCorrelationId);
}

inline void PackSessionFileHeader(std::uint8_t out[kSessionFileHeaderPackedBytes],
                                  std::uint64_t engineBuildId,
                                  std::uint64_t engineMonitorInstanceId,
                                  std::uint64_t createdUtcTicks,
                                  std::uint64_t capabilityFingerprint) {
    std::memset(out, 0, kSessionFileHeaderPackedBytes);
    WriteU32(out, VN_MONITOR_SESSION_FORMAT_MAJOR);
    WriteU32(out + 4, VN_MONITOR_SESSION_FORMAT_MINOR);
    WriteU64(out + 12, VN_MONITOR_SESSION_FILE_MAGIC);
    WriteU64(out + 20, kSessionFileHeaderPackedBytes);
    WriteU64(out + 28, engineBuildId);
    WriteU64(out + 36, engineMonitorInstanceId);
    WriteU64(out + 44, createdUtcTicks);
    WriteU64(out + 52, capabilityFingerprint);
    WriteU32(out + 60, Crc32C(out, kSessionFileHeaderPackedBytes));
}

inline void PackSessionChunk(std::vector<std::uint8_t>* out, std::uint16_t kind,
                             std::uint16_t flags, std::uint64_t chunkSequence,
                             const std::uint8_t* payload, std::uint64_t payloadBytes) {
    const std::size_t start = 0;
    out->resize(VN_MONITOR_SESSION_CHUNK_HEADER_BYTES +
                static_cast<std::size_t>(payloadBytes) +
                VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES);
    std::uint8_t* header = out->data() + start;
    WriteU16(header, kind);
    WriteU16(header + 2, flags);
    WriteU16(header + 4, 1);
    WriteU16(header + 6, static_cast<std::uint16_t>(
                             VN_MONITOR_SESSION_CHUNK_HEADER_BYTES));
    WriteU64(header + 8, payloadBytes);
    WriteU64(header + 16, chunkSequence);
    WriteU32(header + 24, Crc32C(payload, static_cast<std::size_t>(payloadBytes)));
    WriteU32(header + 28, Crc32C(header, 28));
    if (payloadBytes != 0 && payload != nullptr) {
        std::memcpy(header + VN_MONITOR_SESSION_CHUNK_HEADER_BYTES, payload,
                    static_cast<std::size_t>(payloadBytes));
    }
    std::uint8_t* footer =
        header + VN_MONITOR_SESSION_CHUNK_HEADER_BYTES + payloadBytes;
    WriteU64(footer, VN_MONITOR_SESSION_COMMIT_MAGIC);
    WriteU64(footer + 8, chunkSequence);
    WriteU64(footer + 16, payloadBytes);
}

} // namespace monitor
} // namespace harken

#endif
