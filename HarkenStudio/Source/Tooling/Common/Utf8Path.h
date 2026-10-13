#ifndef HARKEN_STUDIO_TOOLING_UTF8_PATH_H
#define HARKEN_STUDIO_TOOLING_UTF8_PATH_H

#include <filesystem>
#include <string>
#include <string_view>

namespace harkenstudiojson {

// Tool manifests and command-line arguments use UTF-8 on every platform.
inline std::filesystem::path Utf8Path(std::string_view text) {
    return std::filesystem::path(std::u8string_view(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline std::string PathUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

} // namespace harkenstudiojson

#endif
