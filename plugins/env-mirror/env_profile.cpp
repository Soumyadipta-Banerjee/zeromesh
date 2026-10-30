#include "plugins/env-mirror/env_profile.hpp"

#include "store/store.hpp"

#include "zeromesh/v1/common.pb.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace zeromesh::mirror {

namespace fs = std::filesystem;
namespace v1 = zeromesh::v1;

std::string EnvProfile::Serialize() const {
    v1::EnvProfile p;
    p.set_root_hash(root_hash);
    for (const auto& e : entries) {
        auto* pe = p.add_entries();
        pe->set_blob(e.blob);
        pe->set_chunk(e.chunk);
        pe->set_offset(e.offset);
    }
    std::string out;
    p.SerializeToString(&out);
    return out;
}

std::optional<EnvProfile> EnvProfile::Deserialize(std::string_view data) {
    v1::EnvProfile p;
    if (!p.ParseFromArray(data.data(), static_cast<int>(data.size()))) return std::nullopt;
    if (static_cast<size_t>(p.entries_size()) > kMaxProfileEntries) return std::nullopt;
    EnvProfile out;
    out.root_hash = p.root_hash();
    out.entries.reserve(static_cast<size_t>(p.entries_size()));
    for (const auto& e : p.entries()) out.entries.push_back({e.blob(), e.chunk(), e.offset()});
    return out;
}

std::string EnvProfile::Id() const {
    return store::CasStore::ComputeDigest(Serialize()).hash;
}

void AccessRecorder::Record(const std::string& blob, uint64_t offset, uint64_t size) {
    if (size == 0 || blob.empty()) return;
    const uint64_t first = offset / kProfileChunkBytes;
    const uint64_t last = (offset + size - 1) / kProfileChunkBytes;
    std::lock_guard<std::mutex> lock(mu_);
    for (uint64_t c = first; c <= last; ++c) {
        if (entries_.size() >= kMaxProfileEntries) return;
        if (!seen_.insert(blob + "#" + std::to_string(c)).second) continue;
        entries_.push_back({blob, static_cast<uint32_t>(c), c == first ? offset : c * kProfileChunkBytes});
    }
}

EnvProfile AccessRecorder::Snapshot(const std::string& root_hash) const {
    std::lock_guard<std::mutex> lock(mu_);
    EnvProfile p;
    p.root_hash = root_hash;
    p.entries = entries_;
    return p;
}

size_t AccessRecorder::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

namespace {

bool WriteAtomic(const fs::path& path, const std::string& data) {
    static std::atomic<uint64_t> counter{0};
    const fs::path tmp = path.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(counter++);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<std::string> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

bool ProfileStore::Save(const EnvProfile& profile, std::string* id) const {
    if (profile.entries.empty() || !store::CasStore::IsValidHash(profile.root_hash)) return false;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) return false;
    const std::string data = profile.Serialize();
    const std::string pid = store::CasStore::ComputeDigest(data).hash;
    if (!WriteAtomic(dir_ / pid, data)) return false;
    if (!WriteAtomic(dir_ / ("root-" + profile.root_hash), pid)) return false;
    if (id) *id = pid;
    return true;
}

std::optional<EnvProfile> ProfileStore::LoadById(const std::string& id) const {
    if (!store::CasStore::IsValidHash(id)) return std::nullopt;
    auto data = ReadAll(dir_ / id);
    if (!data) return std::nullopt;
    if (store::CasStore::ComputeDigest(*data).hash != id) return std::nullopt;
    return EnvProfile::Deserialize(*data);
}

std::optional<EnvProfile> ProfileStore::LoadForRoot(const std::string& root_hash) const {
    if (!store::CasStore::IsValidHash(root_hash)) return std::nullopt;
    auto pointer = ReadAll(dir_ / ("root-" + root_hash));
    if (!pointer) return std::nullopt;
    while (!pointer->empty() && (pointer->back() == '\n' || pointer->back() == ' ')) pointer->pop_back();
    auto p = LoadById(*pointer);
    if (!p || p->root_hash != root_hash) return std::nullopt;
    return p;
}

PrefetchPlan MakePrefetchPlan(const EnvProfile& profile, const std::vector<EnvFile>& env_files) {
    std::unordered_map<std::string, uint64_t> size_of;
    std::vector<std::string> tree_order;
    uint64_t total = 0;
    for (const auto& f : env_files) {
        if (!size_of.emplace(f.hash, f.size).second) continue; // the same blob under two names
        tree_order.push_back(f.hash);
        total += f.size;
    }

    std::vector<std::string> profiled;
    std::unordered_set<std::string> seen;
    uint64_t covered = 0;
    for (const auto& e : profile.entries) {
        auto it = size_of.find(e.blob);
        if (it == size_of.end()) continue; // not in this environment any more
        if (!seen.insert(e.blob).second) continue;
        profiled.push_back(e.blob);
        covered += it->second;
    }

    PrefetchPlan plan;
    plan.coverage = total == 0 ? 0.0 : static_cast<double>(covered) / static_cast<double>(total);
    if (total > 0 && covered * 100 > total * kBulkFetchPercent) {
        plan.whole = true;
        plan.blobs = std::move(tree_order);
        plan.bytes = total;
    } else {
        plan.blobs = std::move(profiled);
        plan.bytes = covered;
    }
    return plan;
}

} // namespace zeromesh::mirror
