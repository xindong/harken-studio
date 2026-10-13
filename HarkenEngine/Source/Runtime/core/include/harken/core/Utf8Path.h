// Non-throwing UTF-8 path construction for the public C ABI. The public path
// contract is UTF-8; on Windows std::filesystem::path transcodes UTF-8 to its
// wchar_t native form and throws std::system_error on ill-formed input. A
// throw escaping the C ABI is undefined behavior, so every naked
// std::u8string_view path construction across the engine funnels through this
// validated, noexcept helper instead.
#ifndef HARKEN_CORE_UTF8_PATH_H
#define HARKEN_CORE_UTF8_PATH_H

#include <filesystem>
#include <string>

namespace harken {

// Validates utf8 as well-formed UTF-8 and, only then, constructs a filesystem
// path from it. Returns false (leaving *out untouched) on a nullptr argument
// or malformed input; never throws. Well-formed input yields the same path a
// direct char8_t construction would, minus the throwing conversion path.
bool TryMakeUtf8Path(const char* utf8, std::filesystem::path* out) noexcept;

// Returns a filesystem path's native UTF-8 spelling without an ANSI-codepage
// round trip. The result uses the path's generic slash form.
std::string Utf8PathString(const std::filesystem::path& path);

} // namespace harken

#endif // HARKEN_CORE_UTF8_PATH_H
