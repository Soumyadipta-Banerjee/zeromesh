#pragma once

#include "zeromesh/v1/common.pb.h"

#include <string>

namespace zeromesh::node {

// Fills unset fields with the documented defaults: policy (may_run_local/remote true,
// max_retries 3, timeout_s 3600, allow_network false, preemptible true),
// working_dir ".", env_spec.name "host". Explicitly set values are kept.
void Normalize(v1::TaskSpec& spec);

// Hex SHA-256 identity of everything that affects a task's result: argv, env,
// input_root, working_dir, outputs, capture_changed, env_spec, resources.arch,
// policy.allow_network and toolchain_digest. Name, dependencies, cores/mem, retries, timeout and placement
// flags are excluded. Computed over a normalized copy of `spec`.
std::string TaskKey(const v1::TaskSpec& spec);

// Digest of the host toolchain `spec` would run on this node (see exec::ToolchainDigest);
// "" for non-host envs, an empty argv or an unresolvable executable.
std::string LocalToolchainDigest(const v1::TaskSpec& spec);

// Records this node's toolchain in `spec` unless the caller already set one, so the
// action-cache key distinguishes results produced by different compilers or libraries.
void StampToolchain(v1::TaskSpec& spec);

} // namespace zeromesh::node
