#pragma once

// Environment capture (`zm env capture`, `--env capture`): the files a program needs, as a tree of
// absolute paths that env-tree mounts read-only on a donor.
//
// Probes run on this machine under strace; every file they open for reading or execute is
// captured. strace never sees what the kernel itself loads to start a program, so each executed
// file adds its #! interpreter and its ELF loader (PT_INTERP). A path given whole is taken as is,
// and each dynamically linked program found in it adds the shared libraries the loader resolves
// for it (`ld.so --list`). Secrets on the deny list are never captured. Only what env-tree mounts
// is kept: /proc, /dev, /tmp, most of /etc, ... and anything under the probe's working directory
// stay out.

#include "store/merkle.hpp"
#include "zeromesh/v1/client.pb.h"

#include <grpcpp/support/status.h>

#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::node {

// Whether env-tree mounts the tree's top-level directory `name` at "/<name>". /etc, /proc, /sys,
// /dev, /tmp, /run, /var, /boot, /root always come from the donor.
bool EnvTreeMounts(std::string_view name);

// The /etc entries a tree may carry; env-tree mounts them instead of the donor's.
inline constexpr std::array<const char*, 3> kEnvTreeEtcPaths = {"/etc/ld.so.cache", "/etc/alternatives", "/etc/ssl"};

// Whether an absolute path lies where env-tree would mount it from a tree.
bool EnvTreeCarries(const std::filesystem::path& abs);

// What a strace log shows a process tree used. The log is strace's
//   -f -y -e status=successful -e trace=open,openat,openat2,execve,execveat
// output. Paths are absolute and normalized; relative ones are resolved against the directory
// strace prints for the dirfd (-y), and dropped where it printed none. Opens for writing and of
// directories are skipped. Each list keeps first-seen order without duplicates.
struct TracedFiles {
    std::vector<std::string> opened;
    std::vector<std::string> executed;
};
TracedFiles ParseStraceLog(std::string_view log);

// What the kernel loads to run `exe` before the first traced syscall: the #! interpreter of a
// script (followed while it is itself a script) and the PT_INTERP loader of a dynamic ELF.
std::vector<std::string> ExecLoaders(const std::filesystem::path& exe);

class EnvCapturer {
public:
    // Stat caches and the last tree of each capture live under <state_dir>/captures.
    EnvCapturer(std::shared_ptr<store::MerkleBuilder> merkle, std::filesystem::path state_dir);

    grpc::Status Capture(const v1::CaptureRequest& req, v1::CaptureReply* reply);

private:
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::filesystem::path dir_;
    std::mutex mu_; // one capture at a time
};

} // namespace zeromesh::node
