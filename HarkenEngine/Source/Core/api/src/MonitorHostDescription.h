#ifndef HARKEN_API_MONITOR_HOST_DESCRIPTION_H
#define HARKEN_API_MONITOR_HOST_DESCRIPTION_H

#include "harken/Harken.h"
#include <cstdint>
#include <cstring>

namespace harken {

// Length-delimited Unicode scalar values. No locale, heap, or host object access.
inline bool ValidMonitorUtf8(const std::uint8_t* bytes, std::uint32_t count) noexcept {
    for (std::uint32_t i = 0; i < count;) {
        const auto first = bytes[i++];
        if (first < 0x80) continue;
        unsigned extra = 0;
        std::uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { extra = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { extra = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { extra = 3; code = first & 0x07; minimum = 0x10000; }
        else return false;
        if (extra > count - i) return false;
        while (extra-- != 0) {
            const auto next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

struct MonitorHostDescription {
    std::uint16_t nameBytes = 0, pathBytes = 0, sceneBytes = 0, flags = 0;
    std::uint8_t name[64]{}, path[256]{}, scene[128]{};

    static bool Capture(const VnMonitorHostRegistrationV2& source, MonitorHostDescription& out) noexcept {
        if (source.nameBytes > sizeof(out.name) || source.pathBytes > sizeof(out.path) ||
            source.sceneBytes > sizeof(out.scene) || (source.descriptionFlags & ~1u) != 0 ||
            !ValidMonitorUtf8(source.name, source.nameBytes) ||
            !ValidMonitorUtf8(source.path, source.pathBytes) ||
            !ValidMonitorUtf8(source.scene, source.sceneBytes)) return false;
        out = {};
        out.nameBytes = source.nameBytes;
        out.pathBytes = source.pathBytes;
        out.sceneBytes = source.sceneBytes;
        out.flags = source.descriptionFlags;
        std::memcpy(out.name, source.name, source.nameBytes);
        std::memcpy(out.path, source.path, source.pathBytes);
        std::memcpy(out.scene, source.scene, source.sceneBytes);
        return true;
    }

    bool Same(const MonitorHostDescription& other) const noexcept {
        return nameBytes == other.nameBytes && pathBytes == other.pathBytes && sceneBytes == other.sceneBytes &&
            flags == other.flags && std::memcmp(name, other.name, nameBytes) == 0 &&
            std::memcmp(path, other.path, pathBytes) == 0 && std::memcmp(scene, other.scene, sceneBytes) == 0;
    }
};
static_assert(sizeof(MonitorHostDescription) == 456);
static_assert(sizeof(VnMonitorHostRegistrationV2) == 512);

} // namespace harken
#endif
