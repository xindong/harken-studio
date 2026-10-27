#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace HarkenSetup {

inline bool unpackPayload(const void* manifest, DWORD manifestSize,
                          const void* blocks, DWORD blocksSize,
                          std::vector<unsigned char>& output)
{
    constexpr size_t headerSize = 44;
    constexpr size_t blockSize = 4096;
    if (!manifest || !blocks || manifestSize < headerSize || !blocksSize
        || blocksSize % blockSize != 0
        || std::memcmp(manifest, "HKMSI001", 8) != 0)
        return false;
    const auto* metadata = static_cast<const unsigned char*>(manifest);
    const auto* data = static_cast<const unsigned char*>(blocks);
    uint32_t size = 0;
    std::memcpy(&size, metadata + 8, sizeof(size));
    const size_t count = (uint64_t(size) + blockSize - 1) / blockSize;
    if (!size || manifestSize != headerSize + count * sizeof(uint32_t))
        return false;
    for (size_t i = 0; i < count; ++i) {
        uint32_t index = 0;
        std::memcpy(&index, metadata + headerSize + i * sizeof(index), sizeof(index));
        if (index >= blocksSize / blockSize)
            return false;
    }
    std::vector<unsigned char> restored;
    try {
        restored.resize(size);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        uint32_t index = 0;
        std::memcpy(&index, metadata + headerSize + i * sizeof(index), sizeof(index));
        const size_t remaining = size - i * blockSize;
        std::memcpy(restored.data() + i * blockSize, data + size_t(index) * blockSize,
                    remaining < blockSize ? remaining : blockSize);
    }
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, restored.data(), size,
                   digest, sizeof(digest)) < 0
        || std::memcmp(digest, metadata + 12, sizeof(digest)) != 0)
        return false;
    output = std::move(restored);
    return true;
}

}
