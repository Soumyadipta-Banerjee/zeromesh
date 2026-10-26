#include "net/discovery.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <random>
#include <system_error>
#include <vector>

namespace zeromesh::net {

namespace {

constexpr std::string_view kAnnounceTag = "ZM3";
constexpr std::string_view kDrainTag = "ZM3D";
constexpr size_t kBootIdHexLen = 32;
constexpr std::string_view kAnnounceLabel = "zm-announce";
constexpr std::string_view kDrainLabel = "zm-drain";
constexpr size_t kMaxPacket = 4096;
constexpr size_t kMaxIdLen = 128;
constexpr size_t kMaxHostLen = 255;
constexpr size_t kMaxArchLen = 64;
constexpr size_t kMaxReasonLen = 1024;

// Control characters (tabs, line breaks, ...) become spaces so a field can never
// split the tab-separated record; the result is capped at `max_len` bytes.
std::string Sanitize(std::string_view in, size_t max_len) {
    std::string out(in.substr(0, max_len));
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = ' ';
    }
    return out;
}

std::vector<std::string_view> SplitTabs(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        const size_t tab = s.find('\t', start);
        if (tab == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, tab - start));
        start = tab + 1;
    }
}

std::string Hex(const std::array<uint8_t, 32>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0f]);
    }
    return out;
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<std::array<uint8_t, 32>> Unhex32(std::string_view s) {
    std::array<uint8_t, 32> out{};
    if (s.size() != out.size() * 2) return std::nullopt;
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = HexNibble(s[2 * i]);
        const int lo = HexNibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

// Appends "\t" + hex(Mac(key, label, body)) to body.
std::string Sign(std::string body, std::string_view label, const TeamKey& key) {
    const auto mac = Mac(key, label, body);
    body.push_back('\t');
    body += Hex(mac);
    return body;
}

// True when the field after the last tab is a valid tag over everything before it.
bool VerifyTag(std::string_view packet, std::string_view label, const TeamKey& key) {
    const size_t last_tab = packet.rfind('\t');
    if (last_tab == std::string_view::npos) return false;
    const auto tag = Unhex32(packet.substr(last_tab + 1));
    if (!tag) return false;
    return MacEqual(Mac(key, label, packet.substr(0, last_tab)), *tag);
}

template <typename T>
std::optional<T> ParseNumber(std::string_view s) {
    T value{};
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, value);
    if (ec != std::errc{} || ptr != end || s.empty()) return std::nullopt;
    return value;
}

template <typename T>
void AppendNumber(std::string& out, T value) {
    std::array<char, 64> buf{};
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    if (ec == std::errc{}) out.append(buf.data(), ptr);
}

std::string_view StateName(PeerState s) {
    return s == PeerState::Draining ? "draining" : "active";
}

// Common envelope checks: size, record tag, field count, team id, MAC, sender id.
std::optional<std::vector<std::string_view>> OpenEnvelope(std::string_view packet,
                                                          std::string_view record_tag,
                                                          size_t field_count,
                                                          std::string_view label,
                                                          const TeamKey& key,
                                                          std::string_view self_node_id) {
    if (packet.size() > kMaxPacket) return std::nullopt;
    auto f = SplitTabs(packet);
    if (f.size() != field_count || f[0] != record_tag) return std::nullopt;
    if (f[1] != TeamId(key)) return std::nullopt;
    if (!VerifyTag(packet, label, key)) return std::nullopt;
    if (f[2].empty() || f[2] == self_node_id) return std::nullopt;
    return f;
}

} // namespace

int64_t UnixMsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string FormatAnnouncement(const AnnounceFields& a, const TeamKey& key) {
    const Telemetry& t = a.telemetry;
    const std::string arch = a.arch.empty() ? std::string("unknown") : Sanitize(a.arch, kMaxArchLen);
    const std::string boot_id = Sanitize(a.boot_id, kBootIdHexLen);
    const int64_t ts = a.timestamp_ms != 0 ? a.timestamp_ms : UnixMsNow();

    std::string body;
    body += kAnnounceTag;
    body += '\t';
    body += TeamId(key);
    body += '\t';
    body += Sanitize(a.node_id, kMaxIdLen);
    body += '\t';
    body += boot_id;
    body += '\t';
    AppendNumber(body, a.seq);
    body += '\t';
    AppendNumber(body, ts);
    body += '\t';
    body += Sanitize(a.hostname, kMaxHostLen);
    body += '\t';
    AppendNumber(body, a.node_port);
    body += '\t';
    body += arch;
    body += '\t';
    AppendNumber(body, t.cores_total);
    body += '\t';
    AppendNumber(body, t.cores_free);
    body += '\t';
    AppendNumber(body, t.mem_available);
    body += '\t';
    AppendNumber(body, std::isfinite(t.cpu_load_pct) ? t.cpu_load_pct : 0.0);
    body += '\t';
    AppendNumber(body, t.battery_pct);
    body += '\t';
    body += t.on_ac ? '1' : '0';
    body += '\t';
    body += StateName(a.state);
    return Sign(std::move(body), kAnnounceLabel, key);
}

std::optional<ParsedAnnouncement> ParseAnnouncement(std::string_view packet, const TeamKey& key,
                                                    std::string_view self_node_id) {
    const auto f = OpenEnvelope(packet, kAnnounceTag, 17, kAnnounceLabel, key, self_node_id);
    if (!f) return std::nullopt;
    const auto& v = *f;

    const auto seq = ParseNumber<uint64_t>(v[4]);
    const auto timestamp_ms = ParseNumber<int64_t>(v[5]);
    const auto node_port = ParseNumber<int>(v[7]);
    const auto cores_total = ParseNumber<int>(v[9]);
    const auto cores_free = ParseNumber<int>(v[10]);
    const auto mem_available = ParseNumber<uint64_t>(v[11]);
    const auto cpu_load = ParseNumber<double>(v[12]);
    const auto battery = ParseNumber<int>(v[13]);
    if (!seq || *seq == 0 || !timestamp_ms || *timestamp_ms <= 0) return std::nullopt;
    if (!node_port || *node_port < 1 || *node_port > 65535) return std::nullopt;
    if (!cores_total || *cores_total < 0 || !cores_free || *cores_free < 0) return std::nullopt;
    if (!mem_available || !cpu_load || !std::isfinite(*cpu_load) || *cpu_load < 0) return std::nullopt;
    if (!battery || *battery < -1 || *battery > 100) return std::nullopt;
    if (v[14] != "0" && v[14] != "1") return std::nullopt;
    if (v[3].empty()) return std::nullopt;

    PeerState state;
    if (v[15] == "active") {
        state = PeerState::Active;
    } else if (v[15] == "draining") {
        state = PeerState::Draining;
    } else {
        return std::nullopt;
    }

    ParsedAnnouncement out;
    out.boot_id = std::string(v[3]);
    out.seq = *seq;
    out.timestamp_ms = *timestamp_ms;
    out.peer.id = std::string(v[2]);
    out.peer.hostname = std::string(v[6]);
    out.peer.node_port = *node_port;
    out.peer.arch = std::string(v[8]);
    out.peer.cores = *cores_total;
    out.peer.cores_free = *cores_free;
    out.peer.mem_available = *mem_available;
    out.peer.cpu_load_pct = *cpu_load;
    out.peer.battery_pct = *battery;
    out.peer.is_charging = v[14] == "1";
    out.peer.state = state;
    return out;
}

std::string FormatDrain(const DrainFields& d, const TeamKey& key) {
    const int64_t ts = d.timestamp_ms != 0 ? d.timestamp_ms : UnixMsNow();
    std::string body;
    body += kDrainTag;
    body += '\t';
    body += TeamId(key);
    body += '\t';
    body += Sanitize(d.node_id, kMaxIdLen);
    body += '\t';
    body += Sanitize(d.boot_id, kBootIdHexLen);
    body += '\t';
    AppendNumber(body, d.seq);
    body += '\t';
    AppendNumber(body, ts);
    body += '\t';
    body += Sanitize(d.reason, kMaxReasonLen);
    return Sign(std::move(body), kDrainLabel, key);
}

std::optional<DrainNotice> ParseDrain(std::string_view packet, const TeamKey& key,
                                      std::string_view self_node_id) {
    const auto f = OpenEnvelope(packet, kDrainTag, 8, kDrainLabel, key, self_node_id);
    if (!f) return std::nullopt;
    const auto seq = ParseNumber<uint64_t>((*f)[4]);
    const auto timestamp_ms = ParseNumber<int64_t>((*f)[5]);
    if (!seq || *seq == 0 || !timestamp_ms || *timestamp_ms <= 0) return std::nullopt;
    if ((*f)[3].empty()) return std::nullopt;
    return DrainNotice{std::string((*f)[2]), std::string((*f)[6]), std::string((*f)[3]), *seq,
                       *timestamp_ms};
}

bool TimestampAcceptable(int64_t unix_ms, std::chrono::milliseconds max_skew) {
    if (unix_ms <= 0) return false;
    const int64_t now = UnixMsNow();
    const int64_t skew = max_skew.count();
    return unix_ms >= now - skew && unix_ms <= now + skew;
}

void ReplayTracker::Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.clear();
}

bool ReplayTracker::Accept(std::string_view node_id, std::string_view boot_id, uint64_t seq,
                           int64_t timestamp_ms) {
    if (!TimestampAcceptable(timestamp_ms) || node_id.empty() || boot_id.empty() || seq == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    auto& st = nodes_[std::string(node_id)];
    if (st.boot_id.empty() || st.boot_id != boot_id) {
        st.boot_id = std::string(boot_id);
        st.last_seq = seq;
        return true;
    }
    if (seq <= st.last_seq) return false;
    st.last_seq = seq;
    return true;
}

Discovery::Discovery(std::shared_ptr<PeerTable> peer_table, DiscoveryConfig config)
    : peer_table_(std::move(peer_table)), config_(std::move(config)) {
    if (config_.hostname.empty()) {
        char hostname_buf[256] = {0};
        config_.hostname = gethostname(hostname_buf, sizeof(hostname_buf) - 1) == 0
                               ? std::string(hostname_buf)
                               : std::string("localhost");
    }
    if (config_.node_id.empty()) {
        std::random_device rd;
        config_.node_id = config_.hostname + "-" + std::to_string(getpid()) + "-" +
                          std::to_string(rd() % 100000);
    }
    // Match what peers see on the wire, so our own announcements are recognised.
    config_.node_id = Sanitize(config_.node_id, kMaxIdLen);

    utsname u{};
    arch_ = uname(&u) == 0 ? std::string(u.machine) : std::string("unknown");

    std::array<uint8_t, 16> boot_bytes{};
    std::random_device rd;
    for (auto& b : boot_bytes) b = static_cast<uint8_t>(rd());
    static constexpr char kDigits[] = "0123456789abcdef";
    boot_id_.reserve(kBootIdHexLen);
    for (uint8_t b : boot_bytes) {
        boot_id_.push_back(kDigits[b >> 4]);
        boot_id_.push_back(kDigits[b & 0x0f]);
    }

    team_ = std::move(config_.team);
    config_.team.reset();
}

Discovery::~Discovery() {
    Stop();
}

bool Discovery::Start() {
    if (running_.exchange(true)) {
        return true;
    }
    listener_thread_ = std::thread(&Discovery::ListenerLoop, this);
    advertiser_thread_ = std::thread(&Discovery::AdvertiserLoop, this);
    return true;
}

void Discovery::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (advertiser_thread_.joinable()) advertiser_thread_.join();
    if (listener_thread_.joinable()) listener_thread_.join();
}

void Discovery::SetTeam(std::optional<TeamKey> team) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(team_mutex_);
        changed = team_.has_value() != team.has_value() ||
                  (team_ && team_->secret != team->secret);
        team_ = std::move(team);
    }
    if (changed && peer_table_) {
        peer_table_->Clear();
        replay_.Clear();
    }
}

std::optional<TeamKey> Discovery::Team() const {
    std::lock_guard<std::mutex> lock(team_mutex_);
    return team_;
}

void Discovery::Send(const std::string& msg) const {
    const int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return;

    const uint8_t ttl = 4;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    const uint8_t loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<uint16_t>(config_.discovery_port));
    if (inet_pton(AF_INET, config_.multicast_group.c_str(), &dest.sin_addr) == 1) {
        sendto(sock, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr*>(&dest),
               sizeof(dest));
    }
    close(sock);
}

void Discovery::SetLocalState(PeerState state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    local_state_ = state;
}

PeerState Discovery::local_state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return local_state_;
}

void Discovery::AnnounceNow() {
    const auto team = Team();
    if (!team) return;

    AnnounceFields a;
    a.node_id = config_.node_id;
    a.hostname = config_.hostname;
    a.node_port = config_.node_port;
    if (config_.telemetry) {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        a.telemetry = config_.telemetry();
    } else {
        a.telemetry.cores_total = config_.cores;
        a.telemetry.cores_free = config_.cores;
        a.telemetry.mem_total = config_.ram_bytes;
        a.telemetry.mem_available = config_.ram_bytes;
    }
    a.arch = a.telemetry.arch.empty() ? arch_ : a.telemetry.arch;
    a.state = local_state();
    a.boot_id = boot_id_;
    a.seq = wire_seq_.fetch_add(1) + 1;
    a.timestamp_ms = NowUnixMs();

    Send(FormatAnnouncement(a, *team));
}

void Discovery::BroadcastNodeDraining(const std::string& reason) {
    const auto team = Team();
    if (!team) return;
    SetLocalState(PeerState::Draining);
    Send(FormatDrain(DrainWireFields(reason), *team));
}

int64_t Discovery::NowUnixMs() const {
    return UnixMsNow();
}

DrainFields Discovery::DrainWireFields(std::string_view reason) {
    DrainFields d;
    d.node_id = config_.node_id;
    d.boot_id = boot_id_;
    d.seq = wire_seq_.fetch_add(1) + 1;
    d.timestamp_ms = NowUnixMs();
    d.reason = std::string(reason);
    return d;
}

void Discovery::AdvertiserLoop() {
    constexpr auto kTick = std::chrono::milliseconds(100);
    while (running_.load()) {
        AnnounceNow();
        if (peer_table_) {
            peer_table_->PruneStale(kPeerPruneAfter);
        }
        // Sleep in short ticks so Stop() returns promptly.
        for (auto slept = std::chrono::milliseconds(0);
             slept < kAnnounceInterval && running_.load(); slept += kTick) {
            std::this_thread::sleep_for(kTick);
        }
    }
}

void Discovery::HandlePacket(std::string_view packet, const std::string& sender_ip) {
    const auto team = Team();
    if (!team || !peer_table_) return;

    if (packet.starts_with("ZM3\t")) {
        auto parsed = ParseAnnouncement(packet, *team, config_.node_id);
        if (!parsed) return;
        if (!replay_.Accept(parsed->peer.id, parsed->boot_id, parsed->seq, parsed->timestamp_ms)) {
            return;
        }
        Peer peer = std::move(parsed->peer);
        if (peer.state == PeerState::Draining) {
            if (auto prev = peer_table_->Get(peer.id)) {
                peer.draining_reason = prev->draining_reason;
            }
        }
        peer.ip_address = sender_ip;
        peer.last_seen = std::chrono::steady_clock::now();
        peer_table_->Upsert(std::move(peer));
    } else if (packet.starts_with("ZM3D\t")) {
        if (auto drain = ParseDrain(packet, *team, config_.node_id)) {
            if (!replay_.Accept(drain->node_id, drain->boot_id, drain->seq, drain->timestamp_ms)) {
                return;
            }
            peer_table_->MarkDraining(drain->node_id, drain->reason);
        }
    }
}

void Discovery::ListenerLoop() {
    const int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return;

    const int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<uint16_t>(config_.discovery_port));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        close(sock);
        return;
    }

    ip_mreq mreq{};
    inet_pton(AF_INET, config_.multicast_group.c_str(), &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    std::array<char, kMaxPacket + 1> buffer{};
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLIN;

    while (running_.load()) {
        if (poll(&pfd, 1, 200) <= 0) continue;

        sockaddr_in sender{};
        socklen_t sender_len = sizeof(sender);
        const ssize_t n = recvfrom(sock, buffer.data(), buffer.size(), 0,
                                   reinterpret_cast<sockaddr*>(&sender), &sender_len);
        // Oversized datagrams are truncated to kMaxPacket + 1 bytes and then rejected.
        if (n <= 0) continue;

        char sender_ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sender.sin_addr, sender_ip, sizeof(sender_ip));
        try {
            HandlePacket(std::string_view(buffer.data(), static_cast<size_t>(n)), sender_ip);
        } catch (const std::exception&) {
            // A hostile LAN packet must never take the listener down.
        }
    }

    close(sock);
}

} // namespace zeromesh::net
