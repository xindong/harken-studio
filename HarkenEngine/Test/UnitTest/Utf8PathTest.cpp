// TryMakeUtf8Path helper: the non-throwing UTF-8 -> filesystem::path funnel
// that every public C-ABI path construction routes through. The scan must
// accept all well-formed UTF-8 (RFC 3629 / Unicode Table 3-7) — a regression
// that rejected everything past 7-bit ASCII would silently refuse legal
// non-ASCII filenames — while still rejecting overlong forms, UTF-16
// surrogates, code points above U+10FFFF, orphan continuation bytes, and
// truncated sequences, and never throwing. This exercises each acceptance
// boundary and its adjacent rejection directly on the helper.
//
// Each byte-sequence literal seals its hex escapes at the closing quote so a
// following character is a separate token, never folded into the preceding
// \x escape.
#include "harken/core/Utf8Path.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

// A path distinct from every case below; a rejection must leave *out at this
// value, and an acceptance must replace it.
const std::filesystem::path kSentinel(
    reinterpret_cast<const char8_t*>("harken-untouched"));

std::filesystem::path ExpectedPath(const char* utf8) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8));
}

TEST(Utf8Path, WellFormedBoundariesAndSamplesAccepted) {
    struct Legal {
        const char* name;
        const char* utf8;
    };
    const Legal legals[] = {
        {"C2 80 lowest two-byte (U+0080)", "\xC2\x80"},
        {"DF BF highest two-byte (U+07FF)", "\xDF\xBF"},
        {"E0 A0 80 lowest non-overlong three-byte (U+0800)", "\xE0\xA0\x80"},
        {"ED 9F BF highest before surrogates (U+D7FF)", "\xED\x9F\xBF"},
        {"EE 80 80 lowest after surrogates (U+E000)", "\xEE\x80\x80"},
        {"EF BF BF highest three-byte (U+FFFF)", "\xEF\xBF\xBF"},
        {"F0 90 80 80 lowest four-byte (U+10000)", "\xF0\x90\x80\x80"},
        {"F4 8F BF BF highest code point (U+10FFFF)", "\xF4\x8F\xBF\xBF"},
        {"Chinese sample", "\xE4\xB8\xAD\xE6\x96\x87"},
        {"emoji sample", "\xF0\x9F\x8E\xB5"},
    };
    for (const Legal& c : legals) {
        std::filesystem::path out = kSentinel;
        EXPECT_TRUE(harken::TryMakeUtf8Path(c.utf8, &out)) << c.name;
        EXPECT_EQ(ExpectedPath(c.utf8), out) << c.name;
    }
}

TEST(Utf8Path, IllFormedSequencesRejectedAndOutUntouched) {
    struct Illegal {
        const char* name;
        const char* utf8;
    };
    const Illegal illegals[] = {
        {"C0 80 overlong two-byte", "\xC0\x80"},
        {"C1 BF overlong two-byte", "\xC1\xBF"},
        {"E0 9F 80 overlong three-byte", "\xE0\x9F\x80"},
        {"ED A0 80 UTF-16 surrogate", "\xED\xA0\x80"},
        {"F0 8F 80 80 overlong four-byte", "\xF0\x8F\x80\x80"},
        {"F4 90 80 80 above U+10FFFF", "\xF4\x90\x80\x80"},
        {"F5 80 80 80 out-of-range lead", "\xF5\x80\x80\x80"},
        {"E4 B8 truncated three-byte", "\xE4\xB8"},
        {"F0 90 80 truncated four-byte", "\xF0\x90\x80"},
        {"80 orphan continuation byte", "\x80"},
    };
    for (const Illegal& c : illegals) {
        std::filesystem::path out = kSentinel;
        EXPECT_FALSE(harken::TryMakeUtf8Path(c.utf8, &out)) << c.name;
        EXPECT_EQ(kSentinel, out)
            << c.name << ": a rejection must leave *out untouched";
    }
}

TEST(Utf8Path, NullArgumentsReturnFalseWithoutCrashing) {
    std::filesystem::path out = kSentinel;
    EXPECT_FALSE(harken::TryMakeUtf8Path(nullptr, &out));
    EXPECT_EQ(kSentinel, out);

    // A nullptr out must be refused before any dereference.
    EXPECT_FALSE(harken::TryMakeUtf8Path("\xC2\x80", nullptr));
}

TEST(Utf8Path, PathStringPreservesUnicodeWithoutCodepageLoss) {
    const char* utf8 =
        "F:/Untiy6\xE6\xB5\x8B\xE8\xAF\x95/SDK\xE6\xB5\x8B\xE8\xAF\x95\xE5\xB7\xA5\xE7\xA8\x8B";
    const std::filesystem::path path = ExpectedPath(utf8);
    EXPECT_EQ(std::string(utf8), harken::Utf8PathString(path));
}

} // namespace
