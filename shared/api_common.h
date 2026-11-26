#pragma once
#include "IR_status.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ir::api
{
struct Error : std::runtime_error
{
    int32_t code;
    Error(int32_t c, const std::string &text) : std::runtime_error(text), code(c)
    {
    }
};
inline void require(bool ok, int32_t code, const char *text)
{
    if (!ok)
        throw Error(code, text ? text : "");
}
inline void require(bool ok, int32_t code, const std::string &text)
{
    if (!ok)
        throw Error(code, text);
}
inline void checkStatus(int32_t status, const char *detail)
{
    if (status != IR_OK)
        throw Error(status, detail ? detail : "");
}
inline void copyText(char *out, int32_t count, const char *text) noexcept
{
    if (!out || count <= 0)
        return;
    const char *src = text ? text : "";
    auto n = std::min(size_t(count - 1), std::strlen(src));
    // If UTF-8 text was cut inside a character, omit the incomplete character.
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xc0) == 0x80)
        --n;
    std::memcpy(out, src, n);
    out[n] = 0;
}
template <class Function> int32_t guarded(char *err, int32_t bytes, Function function) noexcept
{
    if (bytes < 0 || (bytes > 0 && !err))
        return IR_BAD_ARGUMENT;
    copyText(err, bytes, "");
    try
    {
        function();
        return IR_OK;
    }
    catch (const Error &e)
    {
        copyText(err, bytes, e.what());
        return e.code;
    }
    catch (const std::system_error &e)
    {
        copyText(err, bytes, e.what());
        return IR_IO_ERROR;
    }
    catch (const std::invalid_argument &e)
    {
        copyText(err, bytes, e.what());
        return IR_BAD_ARGUMENT;
    }
    catch (const std::exception &e)
    {
        copyText(err, bytes, e.what());
        return IR_COMPUTATION_FAILED;
    }
    catch (...)
    {
        copyText(err, bytes, "Unknown DLL exception");
        return IR_COMPUTATION_FAILED;
    }
}
inline int64_t pixels(int32_t rows, int32_t cols)
{
    require(rows > 0 && cols > 0 && cols <= INT32_MAX / 2 && int64_t(rows) * cols <= INT32_MAX, IR_BAD_ARGUMENT,
            "Invalid image dimensions");
    return int64_t(rows) * cols;
}
inline int64_t stackSize(int64_t n, int32_t frames)
{
    require(frames > 0 && n <= INT64_MAX / frames && uint64_t(n) * frames <= SIZE_MAX / sizeof(double),
            IR_BAD_ARGUMENT, "Invalid frame count or stack size");
    return n * frames;
}
inline void needed(const void *pointer, int64_t count, int64_t required)
{
    require(pointer, IR_NULL_POINTER, "Required buffer is NULL");
    require(count >= required, IR_BUFFER_TOO_SMALL, "Buffer capacity is too small (in elements)");
}
} // namespace ir::api
