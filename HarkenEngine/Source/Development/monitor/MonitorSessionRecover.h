#ifndef HARKEN_API_MONITOR_SESSION_RECOVER_H
#define HARKEN_API_MONITOR_SESSION_RECOVER_H

#include <harken/Harken.h>

#include <cstdint>
#include <vector>

namespace harken {
namespace monitor {

struct RecoveredCommit {
    std::uint64_t chunkSequence = 0;
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
    std::uint32_t visible = 0;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> rawBytes;
};

struct RecoveredPrefixChunk {
    std::vector<std::uint8_t> rawBytes;
};

struct SessionRecoverResult {
    VnResult status = VN_OK;
    std::uint32_t commitsWritten = 0;
    std::uint32_t droppedRequired = 0;
    std::uint32_t droppedOptional = 0;
    std::uint32_t recoveryNeeded = 0;
    std::uint64_t bytesConsumed = 0;
    std::uint64_t fileBytes = 0;
    std::uint64_t lastCompleteChunkSequence = 0;
    std::uint64_t fileMagic = 0;
    std::vector<std::uint8_t> fileHeader;
    std::vector<RecoveredCommit> commits;
    std::vector<RecoveredPrefixChunk> committedPrefix;
};

SessionRecoverResult ScanSessionBytes(const std::uint8_t* input, std::uint64_t inputBytes);
SessionRecoverResult RecoverSessionFile(const char* utf8Path);
VnResult CopyRecoveredSessionFile(const char* utf8Source, const char* utf8Destination);

} // namespace monitor
} // namespace harken

#endif
