#pragma once

// Environments as requirements: what a batch's Env asks of a node, how a node answers, and the
// identity of an environment for cache keys and warm directories.
//
// A host program without a version constraint must be the requester's exact build: same binary,
// interpreter and shared libraries (exec::ToolchainDigest), stamped into Program.digest when the run
// is submitted. A program with a constraint (">=4.0,<5") is accepted on any node whose probe prints a
// satisfying version; the adapter has declared those builds interchangeable. A tree environment
// carries its own software, so only the tree's digest matters.

#include "zeromesh/v1/node.pb.h"
#include "zeromesh/v1/run.pb.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::node {

// Fills Program.digest for every exact program this node has. Throws std::invalid_argument for a
// program the requester itself cannot resolve (the run could never be checked anywhere).
void StampPrograms(v1::Env& env);

// A stable string naming what the environment provides, for cache keys and warm dirs.
std::string EnvIdentity(const v1::Env& env);

// Dotted numeric versions; "4.2.1" < "4.10".
struct Version {
    std::vector<long> parts;
    friend auto operator<=>(const Version&, const Version&) = default;
};
std::optional<Version> ParseVersion(std::string_view text); // the first dotted number in `text`
// True when `version` satisfies every comma-separated clause of `constraint` (>=, >, <=, <, ==, !=).
// nullopt for a malformed constraint.
std::optional<bool> Satisfies(const Version& version, std::string_view constraint);

struct EnvRefusal {
    v1::RejectKind kind{v1::REJECT_ENV};
    std::string reason;
};
// Prints the version of a program on this node (run in a sandbox), or nullopt.
using VersionProbe = std::function<std::optional<std::string>(const v1::Program&, const std::string& resolved)>;

// Why this node cannot provide `env`, or nullopt when it can.
std::optional<EnvRefusal> CheckEnv(const v1::Env& env, const VersionProbe& probe);

} // namespace zeromesh::node
