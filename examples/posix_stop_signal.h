#pragma once

#include "IR_station.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>

namespace ir::examples {

inline std::atomic<bool> stopWindowActive{false};

inline BOOL WINAPI consoleCtrlHandler(DWORD ctrlType)
{
    if (!stopWindowActive.load(std::memory_order_acquire))
        return FALSE;
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        IR_RequestStop();
        return TRUE;
    }
    return FALSE;
}

class StopSignalGuard {
public:
    StopSignalGuard()
    {
        stopWindowActive.store(true, std::memory_order_release);
        installed_ = SetConsoleCtrlHandler(consoleCtrlHandler, TRUE) != 0;
        if (!installed_)
            stopWindowActive.store(false, std::memory_order_release);
    }

    StopSignalGuard(const StopSignalGuard &) = delete;
    StopSignalGuard &operator=(const StopSignalGuard &) = delete;

    ~StopSignalGuard()
    {
        stopWindowActive.store(false, std::memory_order_release);
        if (installed_)
            SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    }

private:
    bool installed_{false};
};

} // namespace ir::examples
#else
#include <atomic>
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <system_error>
#include <thread>

namespace ir::examples {

class PosixStopSignalGuard {
public:
    PosixStopSignalGuard()
    {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGINT);
        sigaddset(&signals_, SIGTERM);

        const int result = pthread_sigmask(SIG_BLOCK, &signals_, &previousMask_);
        if (result != 0)
            throw std::system_error(result, std::generic_category(), "Block process stop signals");

        try {
            signalThread_ = std::thread([this] {
                const timespec timeout{0, 50'000'000};
                while (!stopping_.load(std::memory_order_acquire)) {
                    const int signalNumber = sigtimedwait(&signals_, nullptr, &timeout);
                    if (signalNumber == SIGINT || signalNumber == SIGTERM) {
                        IR_RequestStop();
                        return;
                    }
                    if (signalNumber < 0 && errno != EAGAIN && errno != EINTR)
                        return;
                }
            });
        } catch (...) {
            pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr);
            throw;
        }
    }

    PosixStopSignalGuard(const PosixStopSignalGuard &) = delete;
    PosixStopSignalGuard &operator=(const PosixStopSignalGuard &) = delete;

    ~PosixStopSignalGuard()
    {
        stopping_.store(true, std::memory_order_release);
        if (signalThread_.joinable())
            signalThread_.join();
        pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr);
    }

private:
    sigset_t signals_{};
    sigset_t previousMask_{};
    std::atomic<bool> stopping_{false};
    std::thread signalThread_;
};

using StopSignalGuard = PosixStopSignalGuard;

} // namespace ir::examples
#endif
