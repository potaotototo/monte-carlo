#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mc {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity,
                          std::size_t* max_observed_depth = nullptr)
        : capacity_(capacity), max_observed_depth_(max_observed_depth) {
        if (capacity_ == 0) {
            throw std::invalid_argument("bounded queue capacity must be positive");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool push(T item, std::uint64_t* blocked_ns = nullptr) {
        std::unique_lock lock(mutex_);
        const bool blocked = blocked_ns != nullptr && !closed_ &&
                             queue_.size() >= capacity_;
        const auto wait_started = blocked && blocked_ns != nullptr
                                      ? Clock::now()
                                      : Clock::time_point{};
        not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        record_blocked_time(blocked_ns, blocked, wait_started);
        if (closed_) {
            return false;
        }
        queue_.push_back(std::move(item));
        if (max_observed_depth_ != nullptr) {
            *max_observed_depth_ =
                std::max(*max_observed_depth_, queue_.size());
        }
        not_empty_.notify_one();
        return true;
    }

    bool pop(T& output, std::uint64_t* blocked_ns = nullptr) {
        std::unique_lock lock(mutex_);
        const bool blocked = blocked_ns != nullptr && !closed_ && queue_.empty();
        const auto wait_started = blocked && blocked_ns != nullptr
                                      ? Clock::now()
                                      : Clock::time_point{};
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        record_blocked_time(blocked_ns, blocked, wait_started);
        if (queue_.empty()) {
            return false;
        }
        output = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

private:
    using Clock = std::chrono::steady_clock;

    static void record_blocked_time(std::uint64_t* output,
                                    bool blocked,
                                    Clock::time_point started) noexcept {
        if (output == nullptr) {
            return;
        }
        if (!blocked) {
            *output = 0U;
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started).count();
        *output = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
    }

    std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> queue_;
    std::size_t capacity_;
    std::size_t* max_observed_depth_;
    bool closed_ = false;
};

}  // namespace mc
