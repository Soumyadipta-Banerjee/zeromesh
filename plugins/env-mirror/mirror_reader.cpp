#include "plugins/env-mirror/mirror_reader.hpp"

#include <sys/types.h>

#include <algorithm>
#include <cstring>

namespace zeromesh::mirror {

MirrorReader::MirrorReader(std::shared_ptr<IChunkSource> source, std::shared_ptr<MirrorCache> cache)
    : source_(std::move(source)), cache_(std::move(cache)) {}

void MirrorReader::SetRecorder(std::shared_ptr<AccessRecorder> recorder) {
    std::lock_guard<std::mutex> lock(recorder_mu_);
    recorder_ = std::move(recorder);
}

std::ptrdiff_t MirrorReader::Read(const std::string& hash, uint64_t file_size, uint64_t offset, char* buf,
                                  size_t size) {
    ++read_ops_;
    if (offset >= file_size) return 0;
    size = static_cast<size_t>(std::min<uint64_t>(size, file_size - offset));
    if (size == 0) return 0;

    std::shared_ptr<AccessRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(recorder_mu_);
        recorder = recorder_;
    }
    if (recorder) recorder->Record(hash, offset, size);

    if (cache_) {
        if (auto n = cache_->ReadRange(hash, static_cast<off_t>(offset), buf, size)) {
            ++cache_hits_;
            return static_cast<std::ptrdiff_t>(*n);
        }
    }
    if (!source_) return -1;

    ++on_demand_;
    std::optional<std::string> data = source_->FetchChunk(hash);
    if (!data) return -1;
    if (cache_) cache_->Put(hash, *data);

    if (offset >= data->size()) return 0;
    const size_t tail = data->size() - static_cast<size_t>(offset);
    size = std::min(size, tail);
    std::memcpy(buf, data->data() + offset, size);
    return static_cast<std::ptrdiff_t>(size);
}

std::optional<std::string> MirrorReader::GetObject(const std::string& hash, FetchOrigin origin) {
    if (cache_) {
        if (auto cached = cache_->Get(hash)) {
            if (origin == FetchOrigin::kOnDemand) ++cache_hits_;
            return cached;
        }
    }
    if (!source_) return std::nullopt;
    if (origin == FetchOrigin::kOnDemand) {
        ++on_demand_;
    } else {
        ++prefetch_fetches_;
    }
    std::optional<std::string> data = source_->FetchChunk(hash);
    if (!data) return std::nullopt;
    if (origin == FetchOrigin::kPrefetch) prefetch_bytes_ += data->size();
    if (cache_) cache_->Put(hash, *data);
    return data;
}

bool MirrorReader::Prefetch(const std::string& hash, bool* fetched) {
    if (fetched) *fetched = false;
    if (!cache_) return false;
    if (cache_->Contains(hash)) return true;
    if (!GetObject(hash, FetchOrigin::kPrefetch)) return false;
    if (fetched) *fetched = true;
    return cache_->Contains(hash);
}

MirrorStats MirrorReader::Stats() const {
    MirrorStats s;
    s.read_ops = read_ops_.load();
    s.on_demand_fetches = on_demand_.load();
    s.cache_hits = cache_hits_.load();
    s.prefetch_fetches = prefetch_fetches_.load();
    s.prefetch_bytes = prefetch_bytes_.load();
    return s;
}

} // namespace zeromesh::mirror
