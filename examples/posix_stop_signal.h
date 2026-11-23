#pragma once

#ifndef _WIN32
#include "IR_station.h"

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

} // namespace ir::examples
#endif
