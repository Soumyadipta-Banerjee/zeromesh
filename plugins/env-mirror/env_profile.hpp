#pragma once

// Recorded access-order profile of an environment (v2 blueprint, claim 1, after Starlight and
// REAP). The first run of an environment records which blobs the task read and in what order;
// later runs prefetch exactly those blobs, in that order, while the task is starting. A profile
// that covers more than kBulkFetchPercent of the environment's bytes switches to fetching the
// whole environment instead.
//
// Files are fetched as whole blobs, so a profile entry is (blob, chunk, offset): `chunk` is the
// kProfileChunkBytes-sized piece of the blob that was touched and `offset` the first byte read
// in it. The prefetcher works per blob; chunk and offset are kept for finer fetching later.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace zeromesh::mirror {

inline constexpr uint64_t kProfileChunkBytes = 1024 * 1024;
inline constexpr uint64_t kBulkFetchPercent = 80;    // strictly more than this share of bytes => fetch it all
inline constexpr size_t kMaxProfileEntries = 100000; // a runaway task cannot grow a profile without bound

struct ProfileEntry {
    std::string blob; // SHA-256 hex of the file
    uint32_t chunk{0};
    uint64_t offset{0};
};

struct EnvProfile {
    std::string root_hash; // the environment (root manifest digest) this profile belongs to
    std::vector<ProfileEntry> entries;

    [[nodiscard]] std::string Serialize() const;
    static std::optional<EnvProfile> Deserialize(std::string_view data);
    // SHA-256 hex of Serialize(): names the profile (RunRequest.profile_id).
    [[nodiscard]] std::string Id() const;
};

// Collects first-touch (blob, chunk) pairs in the order a task reads them. Thread-safe.
class AccessRecorder {
public:
    void Record(const std::string& blob, uint64_t offset, uint64_t size);
    [[nodiscard]] EnvProfile Snapshot(const std::string& root_hash) const;
    [[nodiscard]] size_t size() const;

private:
    mutable std::mutex mu_;
    std::unordered_set<std::string> seen_; // "<blob>#<chunk>"
    std::vector<ProfileEntry> entries_;
};

// Profiles on disk. `<dir>/<id>` holds the serialized profile (its name is its SHA-256);
// `<dir>/root-<root_hash>` names the newest profile of an environment.
class ProfileStore {
public:
    explicit ProfileStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

    // False for an empty profile or a root_hash that is not a SHA-256 hex string.
    bool Save(const EnvProfile& profile, std::string* id = nullptr) const;
    [[nodiscard]] std::optional<EnvProfile> LoadForRoot(const std::string& root_hash) const;
    // nullopt unless `id` is a valid hash and the file's content hashes to it.
    [[nodiscard]] std::optional<EnvProfile> LoadById(const std::string& id) const;

private:
    std::filesystem::path dir_;
};

struct EnvFile {
    std::string hash;
    uint64_t size{0};
};

struct PrefetchPlan {
    bool whole{false};              // true: every file of the environment, in tree order
    std::vector<std::string> blobs; // fetch order
    uint64_t bytes{0};              // bytes the plan fetches
    double coverage{0.0};           // share of the environment's bytes the profile touched
};

// `env_files` lists the environment's files in tree order. Blobs the profile names that the
// environment no longer has are dropped.
PrefetchPlan MakePrefetchPlan(const EnvProfile& profile, const std::vector<EnvFile>& env_files);

} // namespace zeromesh::mirror
