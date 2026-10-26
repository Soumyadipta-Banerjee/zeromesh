#include "node/admission.hpp"

#include <sodium.h>

#include <algorithm>
#include <stdexcept>

namespace zeromesh::node {

namespace {

std::string Concat(std::string_view a, std::string_view b, std::string_view c) {
    std::string out;
    out.reserve(a.size() + b.size() + c.size());
    out.append(a).append(b).append(c);
    return out;
}

} // namespace

std::string AdmitNonce() {
    if (sodium_init() < 0) throw std::runtime_error("sodium_init failed");
    std::string nonce(kAdmitNonceBytes, '\0');
    randombytes_buf(nonce.data(), nonce.size());
    return nonce;
}

std::array<uint8_t, 32> AdmitServerMac(const net::TeamKey& key, std::string_view nonce_c, std::string_view fp_server,
                                       std::string_view fp_client) {
    return net::Mac(key, "zm-admit-s", Concat(nonce_c, fp_server, fp_client));
}

std::array<uint8_t, 32> AdmitClientMac(const net::TeamKey& key, std::string_view nonce_s, std::string_view fp_client,
                                       std::string_view fp_server) {
    return net::Mac(key, "zm-admit-c", Concat(nonce_s, fp_client, fp_server));
}

bool AdmitMacMatches(std::string_view received, const std::array<uint8_t, 32>& expected) {
    if (received.size() != expected.size()) return false;
    std::array<uint8_t, 32> got{};
    std::copy(received.begin(), received.end(), got.begin());
    return net::MacEqual(got, expected);
}

void PendingAdmissions::Begin(const std::string& fp_client, std::string node_id, std::string nonce_s) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mu_);
    std::erase_if(pending_, [&](const auto& kv) { return kv.second.expires <= now; });
    if (pending_.size() >= kAdmitMaxPending && !pending_.contains(fp_client)) {
        auto oldest = std::min_element(pending_.begin(), pending_.end(), [](const auto& a, const auto& b) {
            return a.second.expires < b.second.expires;
        });
        pending_.erase(oldest);
    }
    pending_[fp_client] = Entry{std::move(node_id), std::move(nonce_s), now + kAdmitPendingTtl};
}

std::optional<PendingAdmissions::Entry> PendingAdmissions::Take(const std::string& fp_client) {
    std::lock_guard lock(mu_);
    auto it = pending_.find(fp_client);
    if (it == pending_.end()) return std::nullopt;
    Entry e = std::move(it->second);
    pending_.erase(it);
    if (e.expires <= std::chrono::steady_clock::now()) return std::nullopt;
    return e;
}

} // namespace zeromesh::node
