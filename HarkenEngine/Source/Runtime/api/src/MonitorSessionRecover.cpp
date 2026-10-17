#include "MonitorSessionRecover.h"

#include "MonitorRecordPack.h"
#include "MonitorSessionFileWrite.h"
#include "MonitorRecordingCodec.h"

#include "harken/core/Utf8Path.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace harken {
namespace monitor {
namespace {

constexpr char kTraceMagic0 = 'V';
constexpr char kTraceMagic1 = 'T';
constexpr char kTraceMagic2 = 'R';
constexpr char kTraceMagic3 = 'C';

std::uint16_t ReadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(ReadU32(p)) |
           (static_cast<std::uint64_t>(ReadU32(p + 4)) << 32);
}

bool AddOverflow(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return true;
    }
    *out = a + b;
    return false;
}

bool ChunkRequired(std::uint32_t flags) {
    return (flags & VN_MONITOR_CHUNK_REQUIRED) != 0 &&
           (flags & VN_MONITOR_CHUNK_OPTIONAL) == 0;
}

bool ChunkOptional(std::uint32_t flags) {
    return (flags & VN_MONITOR_CHUNK_OPTIONAL) != 0 &&
           (flags & VN_MONITOR_CHUNK_REQUIRED) == 0;
}

bool KnownChunkKind(std::uint32_t kind) {
    return kind >= VN_MONITOR_CHUNK_SESSION_HEADER &&
           kind <= VN_MONITOR_CHUNK_EXECUTION_END;
}

bool LooksLikeTraceFile(const std::uint8_t* input, std::uint64_t inputBytes) {
    return inputBytes >= 4 && input[0] == static_cast<std::uint8_t>(kTraceMagic0) &&
           input[1] == static_cast<std::uint8_t>(kTraceMagic1) &&
           input[2] == static_cast<std::uint8_t>(kTraceMagic2) &&
           input[3] == static_cast<std::uint8_t>(kTraceMagic3);
}

SessionRecoverResult Incompatible() {
    SessionRecoverResult result;
    result.status = VN_MONITOR_INCOMPATIBLE;
    return result;
}

void MarkRecovery(SessionRecoverResult* result) {
    if (result->status == VN_OK) {
        result->recoveryNeeded = 1;
    }
}

} // namespace

SessionRecoverResult ScanSessionBytes(const std::uint8_t* input, std::uint64_t inputBytes) {
    SessionRecoverResult result;
    if (input == nullptr && inputBytes != 0) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    result.fileBytes = inputBytes;
    if (inputBytes > VN_MONITOR_HARD_SESSION_FILE_BYTES) {
        return Incompatible();
    }
    if (inputBytes < kSessionFileHeaderPackedBytes) {
        return Incompatible();
    }
    if (LooksLikeTraceFile(input, inputBytes)) {
        return Incompatible();
    }

    const std::uint32_t major = ReadU32(input);
    const std::uint32_t minor = ReadU32(input + 4);
    if (major != VN_MONITOR_SESSION_FORMAT_MAJOR ||
        minor != VN_MONITOR_SESSION_FORMAT_MINOR) {
        return Incompatible();
    }
    const std::uint64_t magic = ReadU64(input + 12);
    if (magic != VN_MONITOR_SESSION_FILE_MAGIC) {
        return Incompatible();
    }
    const std::uint64_t headerBytes = ReadU64(input + 20);
    if (headerBytes != kSessionFileHeaderPackedBytes) {
        return Incompatible();
    }
    std::uint8_t headerCopy[kSessionFileHeaderPackedBytes];
    std::memcpy(headerCopy, input, kSessionFileHeaderPackedBytes);
    headerCopy[60] = 0;
    headerCopy[61] = 0;
    headerCopy[62] = 0;
    headerCopy[63] = 0;
    if (Crc32C(headerCopy, kSessionFileHeaderPackedBytes) != ReadU32(input + 60)) {
        return Incompatible();
    }
    result.fileMagic = magic;
    result.fileHeader.assign(input, input + kSessionFileHeaderPackedBytes);

    std::uint64_t offset = kSessionFileHeaderPackedBytes;
    std::uint64_t lastSequence = 0;
    bool haveSequence = false;
    bool typed = false, typedEnded = false, legacyRecords = false;
    MonitorRecordingVerifier recording;
    while (offset < inputBytes) {
        const std::uint64_t remaining = inputBytes - offset;
        if (remaining < VN_MONITOR_SESSION_CHUNK_HEADER_BYTES) {
            result.bytesConsumed = offset;
            MarkRecovery(&result);
            return result;
        }
        const std::uint8_t* chunk = input + offset;
        const std::uint32_t kind = ReadU16(chunk);
        const std::uint32_t flags = ReadU16(chunk + 2);
        const std::uint32_t version = ReadU16(chunk + 4);
        const std::uint32_t packedHeaderBytes = ReadU16(chunk + 6);
        const std::uint64_t payloadBytes = ReadU64(chunk + 8);
        const std::uint64_t chunkSequence = ReadU64(chunk + 16);
        const std::uint32_t payloadCrc = ReadU32(chunk + 24);
        const std::uint32_t headerCrc32 = ReadU32(chunk + 28);
        if (version != 1u && kind >= VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR && kind <= VN_MONITOR_CHUNK_EXECUTION_END &&
            ChunkRequired(flags) && packedHeaderBytes == VN_MONITOR_SESSION_CHUNK_HEADER_BYTES && Crc32C(chunk, 28) == headerCrc32)
            return Incompatible();
        if (packedHeaderBytes != VN_MONITOR_SESSION_CHUNK_HEADER_BYTES || version != 1u ||
            Crc32C(chunk, 28) != headerCrc32) {
            result.bytesConsumed = offset;
            MarkRecovery(&result);
            return result;
        }
        if (payloadBytes > VN_MONITOR_HARD_CHUNK_PAYLOAD_BYTES) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.bytesConsumed = offset;
            return result;
        }
        std::uint64_t afterPayload = 0;
        if (AddOverflow(offset, VN_MONITOR_SESSION_CHUNK_HEADER_BYTES, &afterPayload) ||
            AddOverflow(afterPayload, payloadBytes, &afterPayload)) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.bytesConsumed = offset;
            return result;
        }
        std::uint64_t commitEnd = 0;
        if (AddOverflow(afterPayload, VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES, &commitEnd)) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.bytesConsumed = offset;
            return result;
        }
        if (commitEnd > inputBytes) {
            result.bytesConsumed = offset;
            MarkRecovery(&result);
            return result;
        }
        const std::uint8_t* payload = chunk + VN_MONITOR_SESSION_CHUNK_HEADER_BYTES;
        const std::uint8_t* footer = payload + payloadBytes;
        if (ReadU64(footer) != VN_MONITOR_SESSION_COMMIT_MAGIC ||
            ReadU64(footer + 8) != chunkSequence ||
            ReadU64(footer + 16) != payloadBytes) {
            result.bytesConsumed = offset;
            MarkRecovery(&result);
            return result;
        }
        if (haveSequence) {
            std::uint64_t expected = 0;
            if (AddOverflow(lastSequence, 1ull, &expected) || chunkSequence != expected) {
                result.status = VN_MONITOR_INCOMPATIBLE;
                result.bytesConsumed = offset;
                return result;
            }
        }
        RecoveredPrefixChunk prefix;
        prefix.rawBytes.assign(chunk, chunk + (commitEnd - offset));
        result.committedPrefix.push_back(std::move(prefix));
        if (Crc32C(payload, static_cast<std::size_t>(payloadBytes)) != payloadCrc) {
            if (ChunkRequired(flags)) {
                result.droppedRequired += 1;
            } else if (ChunkOptional(flags)) {
                result.droppedOptional += 1;
            }
            haveSequence = true;
            lastSequence = chunkSequence;
            offset = commitEnd;
            result.bytesConsumed = offset;
            MarkRecovery(&result);
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

        bool validRecording = true;
        if (typedEnded && kind != VN_MONITOR_CHUNK_RECOVERY_INDEX) validRecording = false;
        else if (kind == VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR) {
            RecordingDescriptor descriptor{};
            validRecording = required && !typed && !legacyRecords &&
                DecodeRecordingDescriptor(payload, payloadBytes, descriptor) &&
                descriptor.engine == ReadU64(input + 36) && recording.Begin(descriptor);
            typed = validRecording;
        } else if (kind == VN_MONITOR_CHUNK_EXECUTION_RECORD) {
            validRecording = required && typed && recording.Record(payload, payloadBytes);
        } else if (kind == VN_MONITOR_CHUNK_EXECUTION_END) {
            RecordingEnd end{};
            validRecording = required && typed && DecodeRecordingEnd(payload, payloadBytes, end) && recording.End(end);
            typedEnded = validRecording;
        } else if (kind == VN_MONITOR_CHUNK_EVENTS) {
            legacyRecords = true;
            validRecording = !typed;
            for (std::uint64_t at = 0; validRecording && at + kPackedEnvelopeBytes <= payloadBytes;) {
                const auto recordKind = ReadU32(payload + at);
                if (recordKind >= VN_MONITOR_RECORD_EXECUTION_FACT && recordKind <= VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL)
                    validRecording = false;
                const auto length = ReadU64(payload + at + 16);
                if (length < kPackedEnvelopeBytes || length > payloadBytes - at) break;
                at += length;
            }
        }
        if (!validRecording) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.bytesConsumed = offset;
            return result;
        }

        RecoveredCommit commit;
        commit.chunkSequence = chunkSequence;
        commit.kind = kind;
        commit.flags = flags;
        commit.visible = 1;
        commit.payload.assign(payload, payload + payloadBytes);
        commit.rawBytes.assign(chunk, chunk + (commitEnd - offset));
        result.commits.push_back(std::move(commit));
        result.commitsWritten = static_cast<std::uint32_t>(result.commits.size());
        result.lastCompleteChunkSequence = chunkSequence;
        haveSequence = true;
        lastSequence = chunkSequence;
        offset = commitEnd;
        result.bytesConsumed = offset;
    }
    result.bytesConsumed = offset;
    if (result.bytesConsumed < result.fileBytes || result.commitsWritten == 0) {
        MarkRecovery(&result);
    }
    if (typed && (!typedEnded || !recording.Complete())) MarkRecovery(&result);
    return result;
}

SessionRecoverResult RecoverSessionFile(const char* utf8Path) {
    std::filesystem::path path;
    if (!TryMakeUtf8Path(utf8Path, &path)) {
        SessionRecoverResult result;
        result.status = VN_INVALID_ARG;
        return result;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Incompatible();
    }
    if (size > VN_MONITOR_HARD_SESSION_FILE_BYTES) {
        return Incompatible();
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Incompatible();
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size != 0) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!in || static_cast<std::uint64_t>(in.gcount()) != size) {
            return Incompatible();
        }
    }
    return ScanSessionBytes(bytes.empty() ? nullptr : bytes.data(),
                            static_cast<std::uint64_t>(bytes.size()));
}

VnResult CopyRecoveredSessionFile(const char* utf8Source, const char* utf8Destination) {
    std::filesystem::path source;
    std::filesystem::path destination;
    if (!TryMakeUtf8Path(utf8Source, &source) ||
        !TryMakeUtf8Path(utf8Destination, &destination)) {
        return VN_INVALID_ARG;
    }
    std::error_code existsEc;
    const SessionRecoverResult recovered = RecoverSessionFile(utf8Source);
    if (recovered.status != VN_OK) {
        return recovered.status;
    }
    if (recovered.fileHeader.size() != kSessionFileHeaderPackedBytes) {
        return VN_MONITOR_INCOMPATIBLE;
    }

    std::filesystem::path temp = destination;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::filesystem::remove(temp, existsEc);
            return VN_MONITOR_RECORDING_FAILED;
        }
        out.write(reinterpret_cast<const char*>(recovered.fileHeader.data()),
                  static_cast<std::streamsize>(recovered.fileHeader.size()));
        for (const RecoveredPrefixChunk& chunk : recovered.committedPrefix) {
            if (chunk.rawBytes.empty()) {
                continue;
            }
            out.write(reinterpret_cast<const char*>(chunk.rawBytes.data()),
                      static_cast<std::streamsize>(chunk.rawBytes.size()));
        }
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temp, existsEc);
            return VN_MONITOR_RECORDING_FAILED;
        }
    }
#if defined(_WIN32)
    const std::wstring wideTemp = temp.wstring();
    const std::wstring wideDest = destination.wstring();
    if (!MoveFileExW(wideTemp.c_str(), wideDest.c_str(), 0)) {
        const DWORD err = GetLastError();
        std::filesystem::remove(temp, existsEc);
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
            return VN_IN_USE;
        }
        return VN_MONITOR_RECORDING_FAILED;
    }
#else
    if (::link(temp.c_str(), destination.c_str()) != 0) {
        const int err = errno;
        std::filesystem::remove(temp, existsEc);
        if (err == EEXIST) {
            return VN_IN_USE;
        }
        return VN_MONITOR_RECORDING_FAILED;
    }
    std::filesystem::remove(temp, existsEc);
#endif
    return VN_OK;
}

} // namespace monitor
} // namespace harken
