#pragma once

#include "plugin/plugin.hpp"
#include "store/store.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace zeromesh::mirror {

/**
 * @brief Abstract source for file chunks / blobs.
 *
 * Replaces the old dht::KademliaDht dependency from frozen/vfs.
 * In production it is backed by NodeClient::PullBlobs from the requester;
 * in unit tests or local execution it can be backed by CasStore or an in-memory map.
 */
class IChunkSource {
public:
    virtual ~IChunkSource() = default;

    /// Fetches chunk or blob bytes by its SHA-256 hex hash.
    /// Returns std::nullopt if the chunk cannot be retrieved.
    virtual std::optional<std::string> FetchChunk(const std::string& hash) = 0;
};

/**
 * @brief Adapts a plugin::IBlobSource to IChunkSource.
 */
class BlobSourceChunkSource final : public IChunkSource {
public:
    explicit BlobSourceChunkSource(plugin::IBlobSource* blob_source)
        : blob_source_(blob_source) {}

    std::optional<std::string> FetchChunk(const std::string& hash) override {
        if (!blob_source_) return std::nullopt;
        return blob_source_->FetchBlob(hash);
    }

private:
    plugin::IBlobSource* blob_source_;
};

/**
 * @brief In-memory chunk source for testing.
 */
class MemoryChunkSource final : public IChunkSource {
public:
    MemoryChunkSource() = default;

    void Put(const std::string& hash, std::string data) {
        chunks_[hash] = std::move(data);
    }

    std::optional<std::string> FetchChunk(const std::string& hash) override {
        auto it = chunks_.find(hash);
        if (it != chunks_.end()) return it->second;
        return std::nullopt;
    }

    bool HasChunk(const std::string& hash) const {
        return chunks_.contains(hash);
    }

private:
    std::unordered_map<std::string, std::string> chunks_;
};

/**
 * @brief Chunk source backed by local CasStore.
 */
class CasChunkSource final : public IChunkSource {
public:
    explicit CasChunkSource(std::shared_ptr<store::CasStore> store)
        : store_(std::move(store)) {}

    std::optional<std::string> FetchChunk(const std::string& hash) override {
        if (!store_) return std::nullopt;
        return store_->GetBlob(hash);
    }

private:
    std::shared_ptr<store::CasStore> store_;
};

} // namespace zeromesh::mirror
