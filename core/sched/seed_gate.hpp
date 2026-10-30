#pragma once

// Admission control for peer seeding (v2 blueprint, claim 1). A donor fetching a blob from
// another donor costs that source's uplink and, on shared Wi-Fi, everyone's airtime, so:
//   - at most `per_source` sends are in flight from any one peer (TaskVine uses 3), and
//   - at most `lan_wide` seed transfers run at once across the whole LAN (default 2).
// Acquire blocks up to `timeout` for a slot; callers that time out fall back to the requester.

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace zeromesh::sched {

class SeedGate {
public:
    static constexpr int kDefaultPerSource = 3;
    static constexpr int kDefaultLanWide = 2;

    explicit SeedGate(int per_source = kDefaultPerSource, int lan_wide = kDefaultLanWide)
        : per_source_(per_source), lan_wide_(lan_wide) {}

    SeedGate(const SeedGate&) = delete;
    SeedGate& operator=(const SeedGate&) = delete;

    void Configure(int per_source, int lan_wide) {
        {
            std::lock_guard lock(mu_);
            per_source_ = per_source;
            lan_wide_ = lan_wide;
        }
        cv_.notify_all();
    }

    // A held slot; frees it when destroyed.
    class Token {
    public:
        Token(Token&& o) noexcept : gate_(std::exchange(o.gate_, nullptr)), source_(std::move(o.source_)) {}
        Token& operator=(Token&& o) noexcept {
            if (this != &o) {
                Release();
                gate_ = std::exchange(o.gate_, nullptr);
                source_ = std::move(o.source_);
            }
            return *this;
        }
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        ~Token() { Release(); }

    private:
        friend class SeedGate;
        Token(SeedGate* gate, std::string source) : gate_(gate), source_(std::move(source)) {}
        void Release() {
            if (gate_) gate_->Release(source_);
            gate_ = nullptr;
        }
        SeedGate* gate_;
        std::string source_;
    };

    std::optional<Token> Acquire(const std::string& source, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu_);
        const bool ok = cv_.wait_for(lock, timeout, [&] { return CanLocked(source); });
        if (!ok) return std::nullopt;
        ++active_[source];
        ++total_;
        return Token(this, source);
    }

    [[nodiscard]] int Active(const std::string& source) const {
        std::lock_guard lock(mu_);
        auto it = active_.find(source);
        return it == active_.end() ? 0 : it->second;
    }

    [[nodiscard]] int ActiveTotal() const {
        std::lock_guard lock(mu_);
        return total_;
    }

private:
    bool CanLocked(const std::string& source) const {
        auto it = active_.find(source);
        const int mine = it == active_.end() ? 0 : it->second;
        return mine < per_source_ && total_ < lan_wide_;
    }

    void Release(const std::string& source) {
        {
            std::lock_guard lock(mu_);
            auto it = active_.find(source);
            if (it != active_.end() && --it->second <= 0) active_.erase(it);
            if (total_ > 0) --total_;
        }
        cv_.notify_all();
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, int> active_;
    int total_{0};
    int per_source_;
    int lan_wide_;
};

} // namespace zeromesh::sched
