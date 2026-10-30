#pragma once

// `zm doctor`: what keeps this node from running or sharing work, each with the fix. The facts are
// gathered from the live system (GatherSystemFacts plus what the daemon knows); Diagnose turns
// them into findings and is pure, so every message can be tested.

#include "zeromesh/v1/client.pb.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::node {

struct DoctorFacts {
    // Sandbox (from the BatchRunner): why items cannot run here, "" when they can.
    std::string sandbox_problem;
    bool overlay{false}; // inputs reach items through an overlay
    // kernel.apparmor_restrict_unprivileged_userns; nullopt where the knob does not exist.
    std::optional<int> apparmor_restricts_userns;
    // Containment (exec/cgroup.hpp): the cgroup all work runs in and its memory budget, or why work
    // cannot be contained here.
    std::string work_cgroup;
    std::string containment_problem;
    uint64_t memory_budget{0}; // bytes; 0 = none
    int memory_pct{0};         // the donor policy's max_memory_pct behind it
    bool dev_fuse{false};              // /dev/fuse opens read-write
    bool fusermount{false};            // fusermount3 on PATH
    bool strace{false};                // strace on PATH (zm env capture)

    bool in_team{false};
    int node_port{0};             // NodeApi TCP
    int discovery_port{0};        // discovery UDP
    std::string multicast_group;  // discovery
    size_t peers_seen{0};   // nodes announcing on the LAN right now
    size_t peers_pinned{0}; // team members admitted
    std::map<std::string, int64_t> clock_skew_ms; // peer -> skew, for peers refused for it

    std::string store_problem; // the store cannot be written, "" when it can
    uint64_t store_bytes{0};   // in the store now
    uint64_t store_limit{0};   // the cap, 0 = none
    uint64_t disk_free{0};     // free on the store's filesystem
};

// Reads what the system offers: AppArmor's userns knob, /dev/fuse, fusermount3, strace, and the
// store directory's free space and writability. Leaves the daemon's own facts untouched.
void GatherSystemFacts(const std::filesystem::path& store_dir, DoctorFacts* facts);

std::vector<v1::Finding> Diagnose(const DoctorFacts& facts);

} // namespace zeromesh::node
