#include "utf8_path.h"

#ifdef _WIN32
#include <Windows.h>
#include <memory>
#endif

namespace ir::utf8 {

std::string systemErrorText(const std::error_code &code)
{
#ifdef _WIN32
    if (code && code.category() == std::system_category()) {
        wchar_t *wide = nullptr;
        auto length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(code.value()), 0, reinterpret_cast<LPWSTR>(&wide), 0, nullptr);
        const std::unique_ptr<wchar_t, decltype(&LocalFree)> buffer(wide, LocalFree);
        while (length && (wide[length - 1] == L'\r' || wide[length - 1] == L'\n' || wide[length - 1] == L' '))
            --length;
        if (length != 0) {
            const int count = static_cast<int>(length);
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, count, nullptr, 0, nullptr, nullptr);
            if (bytes > 0) {
                std::string text(static_cast<size_t>(bytes), '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, wide, count, text.data(), bytes, nullptr, nullptr) > 0)
                    return text;
            }
        }
        return "Win32 error " + std::to_string(code.value());
    }
#endif
    return code.message();
}

} // namespace ir::utf8
