#include "node/doctor.hpp"

#include "exec/subprocess.hpp"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace zeromesh::node {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kLowDiskBytes = 2ull << 30;

std::string Bytes(uint64_t n) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    size_t u = 0;
    while (v >= 1024 && u + 1 < std::size(kUnits)) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, kUnits[u]);
    return buf;
}

v1::Finding Make(const char* check, v1::Severity severity, std::string message, std::string fix = {}) {
    v1::Finding f;
    f.set_check(check);
    f.set_severity(severity);
    f.set_message(std::move(message));
    f.set_fix(std::move(fix));
    return f;
}

} // namespace

void GatherSystemFacts(const fs::path& store_dir, DoctorFacts* f) {
    {
        std::ifstream in("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
        int v = 0;
        if (in >> v) f->apparmor_restricts_userns = v;
    }
    const int fd = ::open("/dev/fuse", O_RDWR | O_CLOEXEC);
    f->dev_fuse = fd >= 0;
    if (fd >= 0) ::close(fd);
    f->fusermount = !exec::FindOnPath("fusermount3").empty();
    f->strace = !exec::FindOnPath("strace").empty();

    struct statvfs vfs{};
    if (::statvfs(store_dir.c_str(), &vfs) == 0) f->disk_free = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    if (::access(store_dir.c_str(), W_OK) != 0) f->store_problem = "the store " + store_dir.string() + " is not writable";
}

std::vector<v1::Finding> Diagnose(const DoctorFacts& f) {
    std::vector<v1::Finding> out;

    if (f.sandbox_problem.empty()) {
        out.push_back(Make("sandbox", v1::SEVERITY_OK, "bubblewrap runs items in a sandbox"));
    } else {
        std::string fix;
        if (f.sandbox_problem.find("not installed") != std::string::npos ||
            f.sandbox_problem.find("too old") != std::string::npos) {
            fix = "install bubblewrap 0.12 or newer (apt install bubblewrap)";
        } else if (f.apparmor_restricts_userns.value_or(0) != 0) {
            fix = "sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0, and copy "
                  "packaging/sysctl/99-zeromesh-userns.conf to /etc/sysctl.d/ to keep it";
        } else if (f.sandbox_problem.find("setuid") != std::string::npos) {
            fix = "install a bubblewrap that is not setuid (chmod u-s on the binary)";
        }
        out.push_back(Make("sandbox", v1::SEVERITY_ERROR,
                           f.sandbox_problem + ": this node can neither run nor donate work", std::move(fix)));
    }

    if (f.sandbox_problem.empty()) {
        out.push_back(f.overlay ? Make("overlay", v1::SEVERITY_OK, "items see their inputs through an overlay")
                                : Make("overlay", v1::SEVERITY_WARN,
                                       "unprivileged overlayfs is not available: every item copies its inputs",
                                       "use Linux 5.11 or newer"));
    }

    if (!f.cgroup) {
        out.push_back(Make("cgroup", v1::SEVERITY_WARN,
                           "no delegated cgroup v2: memory limits are not enforced and OOM kills are reported as "
                           "plain failures",
                           "run zmd as the systemd user service (packaging/systemd/zmd.service, Delegate=)"));
    } else if (!f.cgroup_memory) {
        out.push_back(Make("cgroup", v1::SEVERITY_WARN,
                           "cgroup " + *f.cgroup + " is delegated without the memory controller: declared memory "
                                                   "limits are not enforced",
                           "add memory to Delegate= in zmd.service"));
    } else {
        out.push_back(Make("cgroup", v1::SEVERITY_OK,
                           "delegated cgroup " + *f.cgroup + ": memory limits, OOM detection and CPU accounting work"));
    }

    if (f.dev_fuse && f.fusermount) {
        out.push_back(Make("fuse", v1::SEVERITY_OK, "captured environments are read lazily over FUSE"));
    } else {
        out.push_back(Make("fuse", v1::SEVERITY_WARN,
                           std::string(f.dev_fuse ? "fusermount3 is not installed" : "/dev/fuse cannot be opened") +
                               ": captured environments are fetched whole before the first item",
                           "apt install fuse3"));
    }

    out.push_back(f.strace ? Make("strace", v1::SEVERITY_OK, "zm env capture can trace a command")
                           : Make("strace", v1::SEVERITY_WARN,
                                  "strace is not installed: zm env capture only takes --path", "apt install strace"));

    if (!f.in_team) {
        out.push_back(Make("team", v1::SEVERITY_WARN, "not in a team: work runs on this node only",
                           "zm team create here, then zm team join '<phrase>' on the other machines"));
    } else if (f.peers_pinned == 0) {
        out.push_back(Make("team", v1::SEVERITY_WARN,
                           "in a team, but no other member has been admitted yet (" + std::to_string(f.peers_seen) +
                               " announcing)",
                           f.peers_seen == 0 ? "run zm team join with this team's phrase on another machine"
                                             : "check that TCP " + std::to_string(f.node_port) + " is open between the machines"));
    } else {
        out.push_back(Make("team", v1::SEVERITY_OK,
                           std::to_string(f.peers_pinned) + " member(s) admitted, " + std::to_string(f.peers_seen) +
                               " announcing now"));
    }
    if (f.in_team && f.peers_seen == 0) {
        out.push_back(Make("discovery", v1::SEVERITY_WARN, "no announcements from other nodes are arriving",
                           "allow UDP " + std::to_string(f.discovery_port) + " and multicast " + f.multicast_group +
                               " between the machines (same subnet)"));
    }

    if (f.clock_skew_ms.empty()) {
        if (f.in_team) out.push_back(Make("clock", v1::SEVERITY_OK, "no clock skew with other members"));
    } else {
        for (const auto& [node, skew] : f.clock_skew_ms) {
            char secs[32];
            std::snprintf(secs, sizeof(secs), "%.1f", std::abs(static_cast<double>(skew)) / 1000.0);
            out.push_back(Make("clock", v1::SEVERITY_ERROR,
                               "node " + node.substr(0, 8) + "'s clock is " + secs + " s " +
                                   (skew > 0 ? "ahead of" : "behind") + " this one: its announcements are refused",
                               "enable NTP on both machines (timedatectl set-ntp true)"));
        }
    }

    if (!f.store_problem.empty()) {
        out.push_back(Make("store", v1::SEVERITY_ERROR, f.store_problem, "fix the permissions of ZM_STATE_DIR"));
    } else {
        std::string msg = Bytes(f.store_bytes) + " in the store" +
                          (f.store_limit > 0 ? " (cap " + Bytes(f.store_limit) + ")" : std::string()) + ", " +
                          Bytes(f.disk_free) + " free on its disk";
        out.push_back(f.disk_free < kLowDiskBytes
                          ? Make("store", v1::SEVERITY_WARN, msg + ": inputs and outputs may not fit",
                                 "free disk space, or set ZM_CAS_MAX_BYTES lower so old blobs are reclaimed sooner")
                          : Make("store", v1::SEVERITY_OK, std::move(msg)));
    }
    return out;
}

} // namespace zeromesh::node
