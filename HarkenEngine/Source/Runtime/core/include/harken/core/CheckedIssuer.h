#ifndef HARKEN_CORE_CHECKED_ISSUER_H
#define HARKEN_CORE_CHECKED_ISSUER_H

#include <cstdint>

namespace harken {

enum class IssueStatus : std::uint8_t {
    Ok = 0,
    Exhausted = 1,
};

struct IssueResult {
    IssueStatus status = IssueStatus::Exhausted;
    std::uint64_t value = 0;
};

inline IssueResult TryIssue(std::uint64_t* next, bool* exhausted) {
    if (next == nullptr || exhausted == nullptr || *exhausted || *next == 0) {
        return {};
    }
    const std::uint64_t issued = *next;
    if (issued == UINT64_MAX) {
        *exhausted = true;
    } else {
        ++*next;
    }
    return {IssueStatus::Ok, issued};
}

} // namespace harken

#endif // HARKEN_CORE_CHECKED_ISSUER_H
