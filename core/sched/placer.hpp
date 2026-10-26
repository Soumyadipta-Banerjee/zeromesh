#pragma once

// Placer (spec §7 step 3, §8): the Executor JobManager uses. It ranks this node
// and the active team peers for each task and tries them in order:
//   - local: hand the task to the LocalExecutor;
//   - remote: OfferTask (a rejection moves on to the next candidate), push the
//     inputs the peer lacks, RunTask while forwarding its events, then pull the
//     outputs (whole trees for directory outputs) and stdout/stderr into the local
//     CAS before reporting the result.
// When every node is full, Place waits for an in-flight slot (render-farm queue)
// instead of failing. Hard rejects (policy/arch/mem) still fail immediately.
// A broken run stream gives FAILED "node <id> lost during run". No retries yet.

#include "net/peer_table.hpp"
#include "net/telemetry.hpp"
#include "node/executor.hpp"
#include "node/local_executor.hpp"
#include "node/node_client.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zeromesh::sched {

class Placer final : public node::Executor {
public:
    // A client for a peer from the PeerTable, or nullptr if none can be made. Clients may
    // be shared between tasks (NodeClient is thread-safe).
    using ClientFactory = std::function<std::shared_ptr<node::NodeClient>(const net::Peer&)>;

    // `local` may be null (this node never runs tasks itself). `telemetry` describes this
    // node; an empty resources.arch is filled with its arch before ranking and offering.
    Placer(std::shared_ptr<node::LocalExecutor> local, std::shared_ptr<net::PeerTable> peers, ClientFactory factory,
           std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
           std::string local_node_id, std::function<net::Telemetry()> telemetry);
    // Cancels every task still placing or running remotely and waits for them.
    ~Placer() override;

    Placer(const Placer&) = delete;
    Placer& operator=(const Placer&) = delete;

    void Execute(const std::string& task_id, const v1::TaskSpec& spec, node::TaskCallbacks cb) override;
    void Cancel(const std::string& task_id) override;

    // Node ids of remote peers that currently have at least one task in flight here.
    [[nodiscard]] std::vector<std::string> NodesWithInFlight() const;

    struct Task;
    struct Shared;

private:
    void Place(const std::shared_ptr<Task>& task);
    void RunLocal(const std::shared_ptr<Task>& task, const v1::TaskSpec& spec);
    void ReapFinishedLocked();

    std::shared_ptr<node::LocalExecutor> local_;
    std::shared_ptr<net::PeerTable> peers_;
    ClientFactory factory_;
    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::string local_id_;
    std::function<net::Telemetry()> telemetry_;
    std::shared_ptr<Shared> shared_;

    struct Worker {
        std::shared_ptr<Task> task;
        std::jthread thread;
    };
    std::mutex workers_mu_;
    std::list<Worker> workers_;
};

} // namespace zeromesh::sched
