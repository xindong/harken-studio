// Pure option parsing for the Windows real-device scheduling measurement.
#ifndef HARKEN_HOST_SCHEDULING_MEASURE_OPTIONS_H
#define HARKEN_HOST_SCHEDULING_MEASURE_OPTIONS_H

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace harkenhost {

enum class SchedulingLoad {
    Light,
    Medium,
    Heavy
};

inline bool ParseUnsignedInRange(const char* text, std::uint32_t maximum,
                                 std::uint32_t* out) {
    if (text == nullptr || *text == '\0' || out == nullptr ||
        *text == '-' || *text == '+') {
        return false;
    }
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value > maximum) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

inline bool ParseSchedulingWorkerCount(const char* text,
                                       std::uint32_t* out) {
    std::uint32_t value = 0;
    if (!ParseUnsignedInRange(text, 8, &value)) {
        return false;
    }
    if (value != 0 && value != 1 && value != 2 && value != 4 &&
        value != 8) {
        return false;
    }
    *out = value;
    return true;
}

inline bool ParseSchedulingLoad(const char* text, SchedulingLoad* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    if (std::strcmp(text, "light") == 0) {
        *out = SchedulingLoad::Light;
        return true;
    }
    if (std::strcmp(text, "medium") == 0) {
        *out = SchedulingLoad::Medium;
        return true;
    }
    if (std::strcmp(text, "heavy") == 0) {
        *out = SchedulingLoad::Heavy;
        return true;
    }
    return false;
}

inline bool ParseSchedulingCpuDuty(const char* text, std::uint32_t* out) {
    std::uint32_t value = 0;
    if (!ParseUnsignedInRange(text, 90, &value)) {
        return false;
    }
    if (value != 0 && value != 50 && value != 90) {
        return false;
    }
    *out = value;
    return true;
}

inline bool ParseSchedulingCpuThreads(const char* text,
                                      std::uint32_t* out) {
    return ParseUnsignedInRange(text, 256, out);
}

} // namespace harkenhost

#endif // HARKEN_HOST_SCHEDULING_MEASURE_OPTIONS_H
