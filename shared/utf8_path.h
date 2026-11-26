#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace ir::utf8 {

std::string systemErrorText(const std::error_code &code);

inline std::filesystem::path pathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string pathToUtf8(const std::filesystem::path &path)
{
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

} // namespace ir::utf8
