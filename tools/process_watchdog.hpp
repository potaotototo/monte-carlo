#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace mc::tool {

inline constexpr std::uint64_t kMaximumWatchdogSeconds = 86'400U;

inline std::optional<std::filesystem::path> executable_candidate(
    std::filesystem::path candidate) {
    std::error_code error;
    if (!candidate.is_absolute()) {
        candidate = std::filesystem::absolute(candidate, error);
        if (error) {
            return std::nullopt;
        }
    }
    candidate = std::filesystem::canonical(candidate, error);
    if (error) {
        return std::nullopt;
    }
    const bool is_regular = std::filesystem::is_regular_file(candidate, error);
    if (error || !is_regular || ::access(candidate.c_str(), X_OK) != 0) {
        return std::nullopt;
    }
    return candidate;
}

// Resolve argv[0] once before forking so child phases always execute the same
// canonical file. A bare argv[0] must be searched with POSIX PATH semantics;
// std::filesystem::absolute would incorrectly interpret it relative to cwd.
inline std::filesystem::path resolve_executable_path(
    std::string_view invocation) {
    if (invocation.empty()) {
        throw std::runtime_error("executable path is empty");
    }
    const std::filesystem::path invoked{std::string(invocation)};
    if (invoked.has_parent_path()) {
        const std::optional<std::filesystem::path> resolved =
            executable_candidate(invoked);
        if (resolved.has_value()) {
            return *resolved;
        }
        throw std::runtime_error("cannot resolve executable path");
    }

    const char* environment_path = std::getenv("PATH");
    if (environment_path == nullptr) {
        throw std::runtime_error(
            "cannot resolve bare executable without PATH");
    }
    std::string_view remaining{environment_path};
    for (;;) {
        const std::size_t separator = remaining.find(':');
        const std::string_view component = remaining.substr(0U, separator);
        const std::filesystem::path directory =
            component.empty() ? std::filesystem::path{"."}
                              : std::filesystem::path{std::string(component)};
        const std::optional<std::filesystem::path> resolved =
            executable_candidate(directory / invoked);
        if (resolved.has_value()) {
            return *resolved;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1U);
    }
    throw std::runtime_error("cannot resolve executable through PATH");
}

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
