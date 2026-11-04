#pragma once

// Placer: the Executor the run engine uses. For each batch it ranks this node and the active team
// peers and tries them in order:
//   - local: hand the batch to the LocalExecutor;
//   - remote: OfferBatch (a rejection moves on to the next candidate), push the inputs the peer
//     lacks (or let it seed them from another donor), RunBatch while forwarding its events, and
//     pull each item's outputs into the local CAS as soon as the donor reports the item.
// Capacity is reserved atomically before Offer/Run so concurrent batches do not all pile onto the
// top-ranked node: a peer takes batches up to the work_cores it announces (and the memory it lets
// work use), less what our batches hold there; this node takes them only through its executor's
// hold, beside what other requesters run here. A rejection is that donor's answer only: a passing
// one (busy, owner at the keyboard, draining, fair share) defers the donor for the retry_after it
// names, any other skips it for this attempt.
//
// One Execute is one attempt. It ends when the batch ran (every item reported, or the donor was
// lost, drained or failed the sandbox), or when no node can run it at all; deciding whether to try
// again is the caller's job. Tail hedging runs a second copy of a batch near the end of a run on a
// donor likely to outlive the first; items reach the caller from whichever copy finishes them.

#include "net/peer_table.hpp"
#include "net/telemetry.hpp"
#include "node/donor_policy.hpp"
#include "node/executor.hpp"
#include "node/local_executor.hpp"
#include "node/node_client.hpp"
#include "sched/seed_gate.hpp"
#include "sched/hazard.hpp"
#include "sched/bandwidth.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <unordered_set>
#include <vector>

namespace zeromesh::sched {

class Placer final : public node::Executor {
public:
    // A client for a peer from the PeerTable, or nullptr if none can be made. Clients may
    // be shared between batches (NodeClient is thread-safe).
    using ClientFactory = std::function<std::shared_ptr<node::NodeClient>(const net::Peer&)>;

    // `local` may be null (this node never runs batches itself). `telemetry` describes this
    // node; an empty resources.arch is filled with its arch before ranking and offering.
    // Registers a PeerTable preempt callback so a donor drain cancels in-flight remote runs
    // and re-queues them.
    Placer(std::shared_ptr<node::LocalExecutor> local, std::shared_ptr<net::PeerTable> peers, ClientFactory factory,
           std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
           std::string local_node_id, std::function<net::Telemetry()> telemetry);
    // Cancels every batch still placing or running remotely and waits for them.
    ~Placer() override;

    Placer(const Placer&) = delete;
    Placer& operator=(const Placer&) = delete;

    void Execute(const std::string& batch_id, const v1::BatchSpec& spec, std::set<std::string> exclude,
                 node::BatchCallbacks cb) override;
    void Cancel(const std::string& batch_id) override;

    // How many batches needing `resources` the team could run at once (total, at least 1), and how
    // many could start now (free): what our batches hold, and our batches still looking for a
    // node, are taken. Where no node has a slot for them at all, one at a time (so the batch fails
    // with why).
    [[nodiscard]] node::Slots SlotsFor(const v1::Resources& resources, v1::Where where) const;

    // Rank donors by expected committed work (slots x E[min(remaining life, batch time)]) from
    // this departure-hazard model. Without a model, or until it has any history, placement
    // ranks by free slots as before. Safe to call while batches are being placed.
    void SetHazardModel(std::shared_ptr<const HazardModel> model);

    // Node ids of remote peers that currently have at least one of our batches in flight.
    [[nodiscard]] std::vector<std::string> NodesWithInFlight() const;

    // Batches currently reserved/running on `node_id` (for tests).
    [[nodiscard]] int InFlightOn(const std::string& node_id) const;

    // Payload bytes this requester has moved to / from each remote node.
    struct NodeTraffic {
        uint64_t bytes_pushed{0};
        uint64_t bytes_pulled{0};
        uint64_t bytes_seeded{0}; // input bytes the node fetched from another donor instead of from us
    };
    [[nodiscard]] std::map<std::string, NodeTraffic> Traffic() const;

    // Peer seeding: a donor that misses a blob fetches it from the least-busy donor already holding
    // it (SeedBlobs) instead of the requester uploading it again. Anything not seeded is pushed.
    struct SeedConfig {
        bool enabled{true};
        int per_source_cap{SeedGate::kDefaultPerSource}; // concurrent sends from one donor
        int lan_cap{SeedGate::kDefaultLanWide};          // concurrent seed transfers on the whole LAN
        std::chrono::milliseconds acquire_timeout{2000}; // wait this long for a slot, then push instead
    };
    void SetSeeding(SeedConfig cfg);

    // Tail hedging: near the end of a run, copy a batch that sits on a donor unlikely to outlive
    // it onto a second donor. Items count from whichever copy finishes them first; when one copy
    // finishes the batch the others are cancelled. Off until `survival` is set.
    struct HedgeConfig {
        // P(the donor is still here `horizon_s` seconds from now); unset means never hedge.
        std::function<double(const std::string& node_id, double horizon_s)> survival;
        double survive_below{0.5};  // hedge when the odds fall below this
        int max_copies{2};          // simultaneous copies of one batch, original included
        int tail_max_running{4};    // "tail": nothing waiting for a slot and at most this many batches running
        double default_horizon_s{120.0}; // assumed remaining runtime; capped by the batch's timeout
        std::chrono::milliseconds check_interval{200};
        std::chrono::milliseconds retry_after{1000}; // pause between two hedge launches for one batch
        // True while this node has work waiting for a slot (the run engine's items not yet in a
        // batch): it is not the tail then, and a copy would take a slot from real work.
        std::function<bool()> backlog;

        static HedgeConfig FromPolicy(const node::DonorPolicy& policy);
    };
    struct HedgeStats {
        uint64_t launched{0};  // hedge copies that obtained a donor slot
        uint64_t won{0};       // hedge copies whose result was used
        uint64_t cancelled{0}; // hedge copies stopped because another copy finished first
        double cpu_seconds{0};        // run time x cores of every hedge copy
        double wasted_cpu_seconds{0}; // the part of it spent by copies that did not win
    };
    void SetHedging(HedgeConfig cfg);
    [[nodiscard]] HedgeStats Hedging() const;
    // Per-peer throughput measured from this requester's own pushes and pulls; Rank uses it to
    // turn missing bytes into transfer time.
    [[nodiscard]] const BandwidthEstimator& Bandwidth() const { return bandwidth_; }

    struct Batch;
    struct Shared;

private:
    void Place(const std::shared_ptr<Batch>& batch);
    // Ends one copy's attempt; a failed primary with hedges still running waits for them.
    void EndAttempt(const std::shared_ptr<Batch>& batch, node::BatchDone d);
    // Runs on this node and waits for it to end.
    node::BatchDone RunLocal(const std::shared_ptr<Batch>& batch);
    // Runs on `node_id` through `client` once the offer was accepted and the inputs pushed. `ran` is
    // called when the donor has finished running it, while its last outputs may still be on the way.
    node::BatchDone RunRemote(const std::shared_ptr<Batch>& batch, const std::string& node_id,
                              const std::shared_ptr<node::NodeClient>& client, const std::function<void()>& ran);
    void OnPeerDrain(const std::string& node_id, const std::string& reason);
    void LaunchWorker(const std::shared_ptr<Batch>& batch);
    // Lets `target` fetch what it misses from donors known to hold it; returns what is still missing.
    std::vector<v1::Digest> SeedFromPeers(const std::string& target, node::NodeClient& client,
                                          std::vector<v1::Digest> missing);
    // Our batches (copies excepted) not on a node yet. Needs shared_->mu.
    [[nodiscard]] int UnplacedLocked() const;
    void NoteHolders(const std::string& node_id, const std::vector<v1::Digest>& digests);
    void AddSeeded(const std::string& node_id, uint64_t bytes);
    void HedgeScan();
    void LaunchHedge(const std::shared_ptr<Batch>& primary, std::set<std::string> exclude, const HedgeConfig& cfg);
    void OnHedgeDone(const std::shared_ptr<Batch>& primary, const std::shared_ptr<Batch>& hedge, node::BatchDone d);
    void CancelHedges(const std::shared_ptr<Batch>& primary);
    void ReapFinishedLocked();

    // Every digest under an input tree and their bytes. A tree is content-addressed, so this is
    // walked once per tree, not for every batch over it (a run's batches share one).
    struct InputTree {
        std::vector<store::Digest> digests;
        int64_t bytes{0};
    };
    [[nodiscard]] std::shared_ptr<const InputTree> InputTreeOf(const v1::Digest& root) const;
    [[nodiscard]] int64_t EstimatedMissingBytes(const std::string& node_id, const std::string& input_root_hash,
                                                int64_t total_bytes) const;
    void NoteInputsPushed(const std::string& node_id, const std::string& input_root_hash, int64_t bytes_pushed,
                          int64_t total_bytes);

    void AddTraffic(const std::string& node_id, uint64_t pushed, uint64_t pulled);

    // Blobs recently pushed to a donor. Offers made before a sibling batch's push lands all
    // report the same blobs as missing; the push lock plus this set sends each one once.
    struct DonorSent {
        std::mutex push_mu;
        std::mutex mu;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> sent;
        std::set<std::string> cached_inputs; // donor has executed this root and supports cached offers
    };
    std::shared_ptr<DonorSent> SentFor(const std::string& node_id);
    static constexpr std::chrono::seconds kSentTtl{30};
    static constexpr size_t kCachedInputsMax = 128;

    std::mutex sent_mu_;
    std::map<std::string, std::shared_ptr<DonorSent>> sent_;

    mutable std::mutex seed_mu_;
    SeedConfig seed_cfg_;
    SeedGate seed_gate_;
    std::mutex holders_mu_;
    std::unordered_map<std::string, std::vector<std::string>> holders_; // blob hash -> donors known to hold it
    static constexpr size_t kHoldersMaxBlobs = 200000;
    static constexpr size_t kHoldersPerBlob = 8;

    mutable std::mutex hedge_mu_;
    HedgeConfig hedge_cfg_;
    HedgeStats hedge_stats_;
    std::jthread hedge_thread_;
    [[nodiscard]] std::shared_ptr<const HazardModel> Hazard() const;
    mutable std::mutex hazard_mu_;
    std::shared_ptr<const HazardModel> hazard_;
    BandwidthEstimator bandwidth_;

    mutable std::mutex traffic_mu_;
    std::map<std::string, NodeTraffic> traffic_;

    mutable std::mutex push_cache_mu_;
    std::unordered_map<std::string, int64_t> push_cache_; // "node|root" -> bytes known on peer
    std::list<std::string> push_cache_lru_;
    static constexpr size_t kPushCacheMax = 128;

    mutable std::mutex trees_mu_;
    mutable std::list<std::pair<std::string, std::shared_ptr<const InputTree>>> trees_; // most recent first
    static constexpr size_t kTreesKept = 16;

    std::shared_ptr<node::LocalExecutor> local_;
    std::shared_ptr<net::PeerTable> peers_;
    ClientFactory factory_;
    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::string local_id_;
    std::function<net::Telemetry()> telemetry_;
    std::shared_ptr<Shared> shared_;

    struct Worker {
        std::shared_ptr<Batch> batch;
        std::jthread thread;
    };
    std::mutex workers_mu_;
    std::list<Worker> workers_;
};

} // namespace zeromesh::sched
