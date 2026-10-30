#include "plugins/env-mirror/mirror_prefetch.hpp"

#include "store/merkle.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace zeromesh::mirror {

std::vector<EnvFile> CollectEnvFiles(MirrorReader& reader, const std::string& root_hash, std::stop_token stop) {
    std::vector<EnvFile> files;
    std::deque<std::string> dirs{root_hash};
    std::unordered_set<std::string> visited;
    while (!dirs.empty() && !stop.stop_requested()) {
        const std::string dir = std::move(dirs.front());
        dirs.pop_front();
        if (!visited.insert(dir).second) continue;
        auto data = reader.GetObject(dir, FetchOrigin::kPrefetch);
        if (!data) continue;
        auto manifest = store::DirectoryManifest::Deserialize(*data);
        if (!manifest) continue;
        for (const auto& f : manifest->files) {
            files.push_back({f.digest.hash, static_cast<uint64_t>(std::max<int64_t>(f.digest.size_bytes, 0))});
        }
        for (const auto& d : manifest->directories) dirs.push_back(d.digest.hash);
    }
    return files;
}

size_t RunPrefetch(const PrefetchPlan& plan, MirrorReader& reader, std::stop_token stop,
                   const std::function<void(const std::string&)>& on_blob) {
    size_t fetched = 0;
    for (const auto& blob : plan.blobs) {
        if (stop.stop_requested()) break;
        bool did_fetch = false;
        reader.Prefetch(blob, &did_fetch);
        if (did_fetch) ++fetched;
        if (on_blob) on_blob(blob);
    }
    return fetched;
}

BackgroundPrefetch::BackgroundPrefetch(EnvProfile profile, std::shared_ptr<MirrorReader> reader)
    : reader_(std::move(reader)) {
    thread_ = std::jthread([this, profile = std::move(profile)](std::stop_token stop) {
        auto files = CollectEnvFiles(*reader_, profile.root_hash, stop);
        if (!stop.stop_requested()) {
            const PrefetchPlan plan = MakePrefetchPlan(profile, files);
            whole_ = plan.whole;
            fetched_ = RunPrefetch(plan, *reader_, stop);
        }
        done_ = true;
    });
}

BackgroundPrefetch::~BackgroundPrefetch() {
    Stop();
}

void BackgroundPrefetch::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

} // namespace zeromesh::mirror
