#ifndef HARKEN_CORE_BANK_VERSION_TOKEN_H
#define HARKEN_CORE_BANK_VERSION_TOKEN_H

#include <cstdint>

namespace harken {

struct BankVersionToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

} // namespace harken

#endif // HARKEN_CORE_BANK_VERSION_TOKEN_H
