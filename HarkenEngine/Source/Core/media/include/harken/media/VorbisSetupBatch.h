#pragma once

#include "harken/core/detail/PackDecodeBudget.h"
#include "harken/media/VorbisMedia.h"
#include <array>
#include <mutex>
#include <span>

struct HvSetup;
struct HvTables;

namespace harken {
// Workers may prepare one setup per distinct header while the directory owns
// the immutable bytes. Only the synchronous activation may take those setups.
class VorbisSetupBatch {
public:
    ~VorbisSetupBatch();
    void Prepare(std::span<const std::uint8_t> blob, detail::PackDecodeCharge& retained);
    HvSetup* Take(const VorbisMedia& media, size_t budget, const HvTables* tables) noexcept;
    class Scope {
    public:
        explicit Scope(VorbisSetupBatch* batch) noexcept;
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        VorbisSetupBatch* previous_;
    };
    static thread_local VorbisSetupBatch* current;
private:
    struct Entry {
        std::span<const std::uint8_t> headers;
        HvSetup* setup = nullptr;
    };
    std::mutex mutex_;
    std::array<Entry, 4> entries_{};
};
}
