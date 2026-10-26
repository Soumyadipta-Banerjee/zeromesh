#pragma once

// Blob streaming for NodeApi PushBlobs/PullBlobs (spec §6.1). An object travels as
// a run of v1::Chunk messages: the first carries the object's digest, the last has
// last=true, and each holds at most kBlobChunkBytes of data. The receiving side
// hashes while it writes and stores an object only if its bytes match the declared
// digest, so a peer can never place content under a name it does not hash to.

#include "store/store.hpp"

#include "zeromesh/v1/common.pb.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zeromesh::node {

inline constexpr size_t kBlobChunkBytes = 1024 * 1024;

// Receives a stream of chunks into a CasStore. Not thread-safe.
class BlobReceiver {
public:
    explicit BlobReceiver(store::CasStore& cas, std::string peer_id = {});
    ~BlobReceiver();

    BlobReceiver(const BlobReceiver&) = delete;
    BlobReceiver& operator=(const BlobReceiver&) = delete;

    // Consumes one chunk. Returns "" on success, else why the stream is invalid (bad
    // framing, an invalid hash, too many bytes or a digest mismatch). After an error
    // the receiver must not be fed again.
    std::string Feed(const v1::Chunk& chunk);
    // Call when the stream ends: "" if it ended between objects, else an error.
    [[nodiscard]] std::string Finish() const;

    // Digests stored (or found already present) so far, in stream order.
    [[nodiscard]] const std::vector<store::Digest>& received() const { return received_; }

private:
    struct Object;

    void Abort();

    store::CasStore& cas_;
    std::string peer_id_;
    std::unique_ptr<Object> current_;
    std::vector<store::Digest> received_;
};

// Streams one stored object as chunks through `write` (which returns false when the
// transport failed). `d` must be a valid digest of an object present in `cas`; the
// digest sent is the stored one. Returns "" on success, else an error.
std::string SendBlob(const store::CasStore& cas, const store::Digest& d,
                     const std::function<bool(const v1::Chunk&)>& write);

} // namespace zeromesh::node
