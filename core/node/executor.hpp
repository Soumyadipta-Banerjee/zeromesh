#pragma once

#include "zeromesh/v1/node.pb.h"
#include "zeromesh/v1/run.pb.h"

#include <functional>
#include <set>
#include <string>
#include <string_view>

namespace zeromesh::node {

// How one attempt at a batch ended, as a whole. Items that reached an outcome were reported
// through on_item already; the rest are what `failure` is about.
struct BatchDone {
    std::string node;                          // where it ran (or was last tried)
    v1::FailureKind failure{v1::FAILURE_NONE}; // NONE: every item was reported
    std::string message;
    bool cancelled{false};                     // stopped on request
    uint64_t bytes_pushed{0};
    uint64_t bytes_pulled{0};
    uint64_t bytes_seeded{0};
};

struct BatchCallbacks {
    std::function<void(const std::string& node)> on_placed;            // a node accepted it
    std::function<void(const std::string& item)> on_started;
    std::function<void(const v1::ItemLog&)> on_log;                     // stream_logs only
    // An item's outcome; its output and log blobs are already in this node's CAS.
    std::function<void(const v1::ItemOutcome&, const std::string& node)> on_item;
    std::function<void(const std::string& item, const v1::Digest& tree)> on_checkpoint;
    // A donor turned the batch down; the attempt goes on elsewhere.
    std::function<void(const std::string& node, v1::RejectKind kind, const std::string& reason)> on_reject;
    std::function<void(BatchDone)> on_done; // exactly once per Execute
};

// Batches of one shape the team can run: at once (total), and right now (free: what this node's own
// batches hold or still wait for is taken).
struct Slots {
    int total{1};
    int free{0};
};

// Runs batches somewhere. Execute is asynchronous and makes one attempt: it may try several nodes
// before one accepts, but once the batch ran (or could be run nowhere) it calls on_done exactly
// once. Retrying is the caller's decision. Cancel of an unknown or finished batch is a no-op.
class Executor {
public:
    virtual ~Executor() = default;
    virtual void Execute(const std::string& batch_id, const v1::BatchSpec& spec, std::set<std::string> exclude,
                         BatchCallbacks cb) = 0;
    virtual void Cancel(const std::string& batch_id) = 0;
};

} // namespace zeromesh::node
