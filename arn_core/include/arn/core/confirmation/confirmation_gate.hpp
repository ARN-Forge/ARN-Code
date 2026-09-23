#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <string>

namespace arn::core {

class ConfirmationGate {
    std::mutex mutex_;
    std::condition_variable cv_;
    const std::string nonce_ =
        std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}());
    unsigned long long sequence_ = 0;
    std::string pending_;
    bool answered_ = false, approved_ = false, cancelled_ = false;

public:
    void reset() {
        std::lock_guard lock(mutex_);
        cancelled_ = false;
    }
    void cancel() {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        cv_.notify_all();
    }
    bool answer(const std::string& id, bool approved) {
        std::lock_guard lock(mutex_);
        if (id.empty() || id != pending_ || answered_ || cancelled_)
            return false;
        answered_ = true;
        approved_ = approved;
        cv_.notify_all();
        return true;
    }
    template <class Emit>
    bool wait(Emit emit, std::chrono::milliseconds timeout = std::chrono::seconds(120)) {
        std::unique_lock lock(mutex_);
        if (cancelled_)
            return false;
        pending_ = "change-" + nonce_ + "-" + std::to_string(++sequence_);
        answered_ = approved_ = false;
        emit(pending_);
        cv_.wait_for(lock, timeout, [&] { return answered_ || cancelled_; });
        const bool result = answered_ && approved_ && !cancelled_;
        pending_.clear();
        return result;
    }
};

} // namespace arn::core
