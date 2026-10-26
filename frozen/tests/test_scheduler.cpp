#include "tests/test_framework.hpp"
#include "scheduler/heuristics.hpp"
#include "scheduler/scheduler.hpp"

TEST(SchedulerHeuristics_CriticalPathDetection) {
    using zeromesh::scheduler::SchedulerHeuristics;

    // Linkers and archivers must be identified as critical path
    ASSERT_TRUE(SchedulerHeuristics::IsCriticalPath({"ld", "main.o", "-o", "bin"}));
    ASSERT_TRUE(SchedulerHeuristics::IsCriticalPath({"/usr/bin/lld", "lib.o"}));
    ASSERT_TRUE(SchedulerHeuristics::IsCriticalPath({"ar", "rcs", "libcore.a", "a.o", "b.o"}));

    // Final compile + link command (no -c flag) is critical path
    ASSERT_TRUE(SchedulerHeuristics::IsCriticalPath({"g++", "main.o", "util.o", "-o", "app"}));
    ASSERT_TRUE(SchedulerHeuristics::IsCriticalPath({"clang++", "main.cpp", "-o", "app"}));

    // Compile-only actions (with -c flag) are NOT critical path (prime offload candidates)
    ASSERT_FALSE(SchedulerHeuristics::IsCriticalPath({"g++", "-c", "main.cpp", "-o", "main.o"}));
    ASSERT_FALSE(SchedulerHeuristics::IsCriticalPath({"clang++", "-c", "engine.cpp", "-o", "engine.o"}));
    ASSERT_FALSE(SchedulerHeuristics::IsCriticalPath({"gcc", "-c", "-O3", "kernel.c"}));
}

TEST(SchedulerHeuristics_CostModel) {
    using zeromesh::scheduler::SchedulerHeuristics;

    // Case 1: Local CPU is almost idle (10%), but input is heavy (20MB) -> Stay local
    ASSERT_FALSE(SchedulerHeuristics::ShouldOffload(
        20 * 1024 * 1024,
        80.0, // remote idle
        10.0, // local load (idle)
        std::chrono::milliseconds(5)
    ));

    // Case 2: Local CPU is under heavy load (90%), remote node is idle (80%), small transfer (500KB) -> Offload
    ASSERT_TRUE(SchedulerHeuristics::ShouldOffload(
        500 * 1024,
        80.0, // remote idle
        90.0, // local load
        std::chrono::milliseconds(2)
    ));

    // Case 3: Remote node has almost no idle headroom (20% idle) -> Do not offload
    ASSERT_FALSE(SchedulerHeuristics::ShouldOffload(
        500 * 1024,
        20.0, // remote idle (too busy)
        80.0, // local load
        std::chrono::milliseconds(2)
    ));
}

TEST(Scheduler_DispatchDecision) {
    auto peer_table = std::make_shared<zeromesh::discovery::PeerTable>();
    zeromesh::scheduler::Scheduler scheduler(peer_table);

    // Critical path task must be pinned locally
    zeromesh::scheduler::ActionTask link_task{
        .action_digest_hash = "link-task-001",
        .command_args = {"g++", "a.o", "b.o", "-o", "app"},
        .env_vars = {},
        .input_root_digest_hash = "digest-link",
        .total_input_bytes = 1000,
        .missing_bytes_on_peer = 0,
        .working_directory = "."
    };

    auto link_decision = scheduler.Decide(link_task, 80.0);
    ASSERT_EQ(link_decision.target, zeromesh::scheduler::DispatchTarget::Local);
    ASSERT_FALSE(link_decision.remote_peer.has_value());

    // Compile task with no active peers must stay local
    zeromesh::scheduler::ActionTask compile_task{
        .action_digest_hash = "compile-task-001",
        .command_args = {"g++", "-c", "math.cpp", "-o", "math.o"},
        .env_vars = {},
        .input_root_digest_hash = "digest-math",
        .total_input_bytes = 5000,
        .missing_bytes_on_peer = 0,
        .working_directory = "."
    };

    auto compile_decision = scheduler.Decide(compile_task, 80.0);
    ASSERT_EQ(compile_decision.target, zeromesh::scheduler::DispatchTarget::Local);
}
