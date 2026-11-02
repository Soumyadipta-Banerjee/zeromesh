#include "tests/unit/test_framework.hpp"

#include "exec/cgroup.hpp"
#include "net/tls.hpp"

#include <iostream>

int main(int /*argc*/, char** argv) {
    // The batch runner tests run contained, like zmd: a delegated scope, asked for before any thread.
    zeromesh::exec::cgroup::EnsureDelegatedScope(argv);
    if (const std::string why = zeromesh::exec::cgroup::ContainmentProblem(); !why.empty()) {
        std::cerr << "WARNING: work cannot be contained here (" << why << "): every test that runs a batch is skipped\n";
    }
    zeromesh::net::PrepareGrpcRuntime();
    return zeromesh::testing::TestRegistry::Instance().RunAll();
}
