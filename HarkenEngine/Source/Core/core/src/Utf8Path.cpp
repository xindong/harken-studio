#include "harken/core/Utf8Path.h"

#include <string_view>

namespace harken {

bool TryMakeUtf8Path(const char* utf8, std::filesystem::path* out) noexcept {
    if (utf8 == nullptr || out == nullptr) {
        return false;
    }

    // Well-formed UTF-8 scan following RFC 3629 and the Unicode Standard's
    // Table 3-7 ("Well-Formed UTF-8 Byte Sequences"). The per-lead-byte legal
    // range of the first trailing byte is what makes the scan reject overlong
    // encodings (0xC0/0xC1 leads and the tightened 0xE0/0xF0 second byte),
    // UTF-16 surrogates U+D800..U+DFFF (the tightened 0xED second byte), and
    // code points above U+10FFFF (0xF4 second byte plus the 0xF5..0xFF leads).
    // Orphan continuation bytes (0x80..0xBF as a lead) and truncated sequences
    // (a required trailing byte reads as the NUL terminator, which fails its
    // range check) are rejected the same way. No exception drives control flow.
    const auto* p = reinterpret_cast<const unsigned char*>(utf8);
    while (*p != '\0') {
        const unsigned char lead = p[0];
        if (lead < 0x80u) { // U+0000..U+007F: a single ASCII byte.
            p += 1;
            continue;
        }
        unsigned char trail1Lo;
        unsigned char trail1Hi; // legal range of the first trailing byte
        int extraTrailing;      // trailing bytes beyond the first
        if (lead >= 0xC2u && lead <= 0xDFu) {        // U+0080..U+07FF
            trail1Lo = 0x80u;
            trail1Hi = 0xBFu;
            extraTrailing = 0;
        } else if (lead == 0xE0u) {                  // U+0800..U+0FFF, no overlong
            trail1Lo = 0xA0u;
            trail1Hi = 0xBFu;
            extraTrailing = 1;
        } else if (lead >= 0xE1u && lead <= 0xECu) { // U+1000..U+CFFF
            trail1Lo = 0x80u;
            trail1Hi = 0xBFu;
            extraTrailing = 1;
        } else if (lead == 0xEDu) {                  // U+D000..U+D7FF, no surrogates
            trail1Lo = 0x80u;
            trail1Hi = 0x9Fu;
            extraTrailing = 1;
        } else if (lead >= 0xEEu && lead <= 0xEFu) { // U+E000..U+FFFF
            trail1Lo = 0x80u;
            trail1Hi = 0xBFu;
            extraTrailing = 1;
        } else if (lead == 0xF0u) {                  // U+10000..U+3FFFF, no overlong
            trail1Lo = 0x90u;
            trail1Hi = 0xBFu;
            extraTrailing = 2;
        } else if (lead >= 0xF1u && lead <= 0xF3u) { // U+40000..U+FFFFF
            trail1Lo = 0x80u;
            trail1Hi = 0xBFu;
            extraTrailing = 2;
        } else if (lead == 0xF4u) {                  // U+100000..U+10FFFF, capped
            trail1Lo = 0x80u;
            trail1Hi = 0x8Fu;
            extraTrailing = 2;
        } else {
            return false; // 0x80..0xC1 orphan/overlong lead, 0xF5..0xFF out of range
        }
        // The NUL terminator (0x00) is below every trailing range, so a
        // truncated sequence fails here before p is ever advanced past it.
        const unsigned char trail1 = p[1];
        if (trail1 < trail1Lo || trail1 > trail1Hi) {
            return false;
        }
        for (int i = 0; i < extraTrailing; ++i) {
            const unsigned char trail = p[2 + i];
            if (trail < 0x80u || trail > 0xBFu) {
                return false;
            }
        }
        p += 2 + extraTrailing;
    }

    // Validated well-formed UTF-8 transcodes without the throwing conversion
    // path. The guard keeps the noexcept promise against the only residual
    // throw source — the path's own allocation.
    try {
        *out = std::filesystem::path(std::u8string_view(
            reinterpret_cast<const char8_t*>(utf8)));
    } catch (...) {
        return false;
    }
    return true;
}

std::string Utf8PathString(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    std::string result(reinterpret_cast<const char*>(encoded.data()),
                       encoded.size());
    for (char& character : result) {
        if (character == '\\') {
            character = '/';
        }
    }
    return result;
}

} // namespace harken
