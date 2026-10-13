#ifndef HARKEN_CORE_DECISION_TOKEN_H
#define HARKEN_CORE_DECISION_TOKEN_H

#include <cstdint>

namespace harken {

struct DecisionToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

struct ParentAudioGroupRef {
    DecisionToken decision{};

    bool Valid() const noexcept { return decision.Valid(); }
};

struct ArmGroupToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

} // namespace harken

#endif // HARKEN_CORE_DECISION_TOKEN_H
