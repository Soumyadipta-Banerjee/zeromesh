#pragma once

#include "zeromesh/v1/common.pb.h"

#include <string>

namespace zeromesh::node {

// Fills unset fields with the documented defaults: policy (may_run_local/remote true,
// max_retries 3, timeout_s 3600, allow_network false, preemptible true),
// working_dir ".", env_spec.name "host". Explicitly set values are kept.
void Normalize(v1::TaskSpec& spec);

// Hex SHA-256 identity of everything that affects a task's result: argv, env,
// input_root, working_dir, outputs, capture_changed, env_spec, resources.arch and
// policy.allow_network. Name, dependencies, cores/mem, retries, timeout and placement
// flags are excluded. Computed over a normalized copy of `spec`.
std::string TaskKey(const v1::TaskSpec& spec);

} // namespace zeromesh::node
