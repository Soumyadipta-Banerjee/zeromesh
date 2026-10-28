#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace zeromesh::run {

// The events of one run for one watcher: replayed history first, then live ones. Next returns
// nullopt on timeout, after Close, or once the stream ended and every event was read.
template <class T>
class EventStream {
public:
    std::optional<T> Next(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu_);
        cv_.wait_for(lock, timeout, [this] { return closed_ || ended_ || !queue_.empty(); });
        if (closed_ || queue_.empty()) return std::nullopt;
        T ev = std::move(queue_.front());
        queue_.pop_front();
        return ev;
    }
    void Close() {
        {
            std::lock_guard lock(mu_);
            closed_ = true;
            queue_.clear();
        }
        cv_.notify_all();
    }
    void Push(const T& ev) {
        {
            std::lock_guard lock(mu_);
            if (closed_ || ended_) return;
            queue_.push_back(ev);
        }
        cv_.notify_all();
    }
    void End() {
        {
            std::lock_guard lock(mu_);
            ended_ = true;
        }
        cv_.notify_all();
    }
    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mu_);
        return closed_;
    }
    [[nodiscard]] bool ended() const {
        std::lock_guard lock(mu_);
        return ended_ && queue_.empty();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool ended_{false};
    bool closed_{false};
};

} // namespace zeromesh::run
