// Command-line value parsing for the host demo, kept as pure functions
// so the parsing contract is unit-testable without running the tool.
#ifndef HARKEN_HOST_DEMO_OPTIONS_H
#define HARKEN_HOST_DEMO_OPTIONS_H

#include <cmath>
#include <cstdlib>

namespace harkenhost {

// Upper bound for --seconds: an hour of demo is far beyond any use of
// this tool, and the bound keeps downstream duration-to-frame and
// tick-count conversions comfortably inside integer range.
inline constexpr double kMaxDemoSeconds = 3600.0;

// Parses a --seconds value: the WHOLE string must convert (no trailing
// junk) to a finite number in (0, kMaxDemoSeconds]. Anything else —
// empty, non-numeric, partial, inf/nan spellings, overflow to infinity,
// zero, negative, over the cap — is refused and *outSeconds is left
// untouched.
inline bool ParseSeconds(const char* text, double* outSeconds) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    if (!std::isfinite(value) || !(value > 0.0) ||
        value > kMaxDemoSeconds) {
        return false;
    }
    *outSeconds = value;
    return true;
}

} // namespace harkenhost

#endif // HARKEN_HOST_DEMO_OPTIONS_H
