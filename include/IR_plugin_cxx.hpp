#pragma once

#include "IR_plugin_abi.h"
#include <cstring>
#include <exception>

/*
 * Optional C++ helper header for metric plugin implementations so C++ exceptions
 * never unwind across the extern "C" ABI boundary.
 */
template <class Fn>
inline int32_t ir_plugin_guard(char *err, int32_t err_bytes, Fn &&fn) noexcept
{
    if (err && err_bytes > 0)
        err[0] = '\0';
    auto copy_err = [err, err_bytes](const char *msg) noexcept {
        if (!err || err_bytes <= 0 || !msg)
            return;
        size_t len = std::strlen(msg);
        const size_t max_len = static_cast<size_t>(err_bytes - 1);
        if (len > max_len)
            len = max_len;
        std::memcpy(err, msg, len);
        err[len] = '\0';
    };
    try {
        return fn();
    } catch (const std::exception &e) {
        copy_err(e.what());
        return IR_COMPUTATION_FAILED;
    } catch (...) {
        copy_err("Unhandled C++ exception in plugin");
        return IR_COMPUTATION_FAILED;
    }
}
