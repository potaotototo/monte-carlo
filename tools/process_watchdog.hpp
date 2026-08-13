#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace mc::tool {

inline constexpr std::uint64_t kMaximumWatchdogSeconds = 86'400U;

inline std::chrono::seconds checked_watchdog_timeout(
    std::uint64_t seconds) {
    if (seconds == 0U || seconds > kMaximumWatchdogSeconds) {
        throw std::invalid_argument(
            "timeout_seconds must be in [1, 86400]");
    }
    return std::chrono::seconds{static_cast<std::chrono::seconds::rep>(seconds)};
}

inline int wait_for_child(pid_t child,
                          std::chrono::steady_clock::duration timeout,
                          std::string_view phase) {
    if (child <= 0) {
        throw std::invalid_argument("watchdog requires a positive child PID");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            break;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("cannot wait for " + std::string(phase) +
                                     " child");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (::kill(child, SIGKILL) != 0 && errno != ESRCH) {
                throw std::runtime_error("cannot terminate timed-out " +
                                         std::string(phase) + " child");
            }
            pid_t reaped = -1;
            do {
                reaped = ::waitpid(child, &status, 0);
            } while (reaped < 0 && errno == EINTR);
            if (reaped != child && !(reaped < 0 && errno == ECHILD)) {
                throw std::runtime_error("cannot reap timed-out " +
                                         std::string(phase) + " child");
            }
            throw std::runtime_error(std::string(phase) +
                                     " child exceeded its watchdog timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!WIFEXITED(status)) {
        const std::string detail =
            WIFSIGNALED(status)
                ? " (signal " + std::to_string(WTERMSIG(status)) + ")"
                : std::string{};
        throw std::runtime_error(std::string(phase) +
                                 " child did not exit normally" + detail);
    }
    return WEXITSTATUS(status);
}

}  // namespace mc::tool
