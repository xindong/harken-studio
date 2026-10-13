#ifndef HARKEN_HOST_PACK_PLAY_PATH_H
#define HARKEN_HOST_PACK_PLAY_PATH_H

#include <filesystem>
#include <string>
#include <system_error>

namespace harkenhost {

inline bool PackPlayLocatorLegal(const std::string& locator) {
    if (locator.empty() || locator.find('\\') != std::string::npos ||
        locator.find(':') != std::string::npos || locator[0] == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start <= locator.size()) {
        const std::size_t slash = locator.find('/', start);
        const std::string part = locator.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

inline bool PackPlayJoinContained(const std::filesystem::path& root,
                                  const std::string& relative,
                                  std::filesystem::path* out) {
    if (out == nullptr || !PackPlayLocatorLegal(relative)) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(
        canonicalRoot / std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(relative.data()), relative.size())), ec);
    if (ec) {
        return false;
    }
    auto resolvedText = resolved.generic_u8string();
    auto rootText = canonicalRoot.generic_u8string();
    if (!rootText.empty() && rootText.back() == '/') {
        rootText.pop_back();
    }
    if (resolvedText != rootText &&
        (resolvedText.size() <= rootText.size() ||
         resolvedText.compare(0, rootText.size(), rootText) != 0 ||
         resolvedText[rootText.size()] != '/')) {
        return false;
    }
    *out = resolved;
    return true;
}

} // namespace harkenhost

#endif
