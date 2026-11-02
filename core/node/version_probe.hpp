#pragma once

// Prints the version of a host program by running its probe in a host sandbox (never on the bare
// host: the requester names the program and its arguments). The answer is remembered per program
// build, so a node probes each build once while it runs.

#include "exec/batch_runner.hpp"
#include "plugin/loader.hpp"
#include "zeromesh/v1/run.pb.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace zeromesh::node {

class VersionProber {
public:
    VersionProber(std::shared_ptr<exec::BatchRunner> runner, std::shared_ptr<plugin::Registry> registry,
                  std::string node_id);

    // The probe's output (stdout, else stderr), or nullopt when it could not run.
    std::optional<std::string> operator()(const v1::Program& program, const std::string& resolved);

private:
    std::shared_ptr<exec::BatchRunner> runner_;
    std::shared_ptr<plugin::Registry> registry_;
    std::string node_id_;
    std::mutex mu_;
    std::map<std::string, std::optional<std::string>> seen_; // build digest + probe -> output
};

} // namespace zeromesh::node
