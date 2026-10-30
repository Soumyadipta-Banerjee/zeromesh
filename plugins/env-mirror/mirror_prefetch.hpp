#pragma once

// Streams a recorded profile into the mirror cache ahead of the task that will read it.

#include "plugins/env-mirror/env_profile.hpp"
#include "plugins/env-mirror/mirror_reader.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace zeromesh::mirror {

// Every file of the environment under `root_hash`, in tree order (a directory's files, then its
// subdirectories, breadth first). Manifests are fetched as prefetch traffic, not on-demand.
std::vector<EnvFile> CollectEnvFiles(MirrorReader& reader, const std::string& root_hash, std::stop_token stop = {});

// Fetches plan.blobs in order, skipping blobs already cached. Returns how many blobs this call
// fetched. `on_blob` is called for each blob handled, in order.
size_t RunPrefetch(const PrefetchPlan& plan, MirrorReader& reader, std::stop_token stop,
                   const std::function<void(const std::string&)>& on_blob = nullptr);

// Plans from `profile` and prefetches on a background thread; the destructor stops and joins it.
class BackgroundPrefetch {
public:
    BackgroundPrefetch(EnvProfile profile, std::shared_ptr<MirrorReader> reader);
    ~BackgroundPrefetch();

    BackgroundPrefetch(const BackgroundPrefetch&) = delete;
    BackgroundPrefetch& operator=(const BackgroundPrefetch&) = delete;

    void Stop();
    [[nodiscard]] bool done() const { return done_.load(); }
    [[nodiscard]] size_t fetched() const { return fetched_.load(); }
    [[nodiscard]] bool whole() const { return whole_.load(); }

private:
    std::shared_ptr<MirrorReader> reader_;
    std::atomic<bool> done_{false};
    std::atomic<bool> whole_{false};
    std::atomic<size_t> fetched_{0};
    std::jthread thread_; // last: destroyed (and joined) first
};

} // namespace zeromesh::mirror
