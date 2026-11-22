#ifndef HARKEN_CORE_AUTHORED_STOP_OPERATION_IDENTITY_H
#define HARKEN_CORE_AUTHORED_STOP_OPERATION_IDENTITY_H

#include <cstdint>

namespace harken {

struct AuthoredStopOperationIdentity {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
    std::uint64_t root = 0;
    std::uint64_t sourceEpoch = 0;
    std::uint64_t actionFiring = 0;
    std::uint32_t executionOrdinal = UINT32_MAX;
    std::uint64_t stopFrame = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0 && root != 0 &&
            sourceEpoch != 0 && actionFiring != 0 && executionOrdinal != UINT32_MAX;
    }
    bool operator==(const AuthoredStopOperationIdentity&) const = default;
};

} // namespace harken

#endif // HARKEN_CORE_AUTHORED_STOP_OPERATION_IDENTITY_H
