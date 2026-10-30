#include "exec/bwrap.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <charconv>
#include <cstdio>
#include <sstream>

namespace zeromesh::exec {

namespace {

// stdout of `binary --version`, or "" if it cannot be run.
std::string VersionBanner(const std::string& binary) {
    int fds[2];
    if (pipe(fds) != 0) return "";
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return "";
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(binary.c_str(), binary.c_str(), "--version", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buf[256];
    for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) > 0;) out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? out : "";
}

} // namespace

std::optional<BwrapVersion> ParseBwrapVersion(const std::string& output) {
    static constexpr char kPrefix[] = "bubblewrap ";
    const auto pos = output.find(kPrefix);
    if (pos == std::string::npos) return std::nullopt;
    const char* cur = output.c_str() + pos + sizeof(kPrefix) - 1;
    const char* const end = output.c_str() + output.size();
    int parts[3];
    for (int i = 0; i < 3; ++i) {
        const auto [next, ec] = std::from_chars(cur, end, parts[i]);
        if (ec != std::errc{}) return std::nullopt;
        cur = next;
        if (i == 2) break;
        if (cur == end || *cur != '.') return std::nullopt;
        ++cur;
    }
    return BwrapVersion{parts[0], parts[1], parts[2]};
}

std::string CheckBwrapBinary(const std::string& binary) {
    struct stat st{};
    if (stat(binary.c_str(), &st) != 0) return "bubblewrap '" + binary + "' is not accessible";
    if (st.st_mode & S_ISUID) return "bubblewrap '" + binary + "' is setuid; refusing to run tasks under it";

    const auto version = ParseBwrapVersion(VersionBanner(binary));
    if (!version) return "cannot determine the version of bubblewrap '" + binary + "'";
    if (!(*version >= kMinBwrapVersion)) {
        std::ostringstream ss;
        ss << "bubblewrap " << version->major << '.' << version->minor << '.' << version->patch << " is too old; need "
           << kMinBwrapVersion.major << '.' << kMinBwrapVersion.minor << '.' << kMinBwrapVersion.patch << " or newer";
        return ss.str();
    }
    return "";
}

BwrapBuilder& BwrapBuilder::SetBinary(std::string path) {
    binary_ = std::move(path);
    return *this;
}
BwrapBuilder& BwrapBuilder::ShareNetwork(bool share) {
    share_net_ = share;
    return *this;
}

BwrapBuilder& BwrapBuilder::AddReadOnlyBind(const std::string& host, const std::string& sandbox) {
    mounts_.push_back({"--ro-bind", host, sandbox});
    return *this;
}

BwrapBuilder& BwrapBuilder::AddReadWriteBind(const std::string& host, const std::string& sandbox) {
    mounts_.push_back({"--bind", host, sandbox});
    return *this;
}

BwrapBuilder& BwrapBuilder::AddSymlink(const std::string& target, const std::string& link) {
    mounts_.push_back({"--symlink", target, link});
    return *this;
}

BwrapBuilder& BwrapBuilder::AddTmpfs(const std::string& sandbox) {
    mounts_.push_back({"--tmpfs", sandbox});
    return *this;
}

BwrapBuilder& BwrapBuilder::AddPerms(unsigned mode) {
    pending_perms_ = mode;
    return *this;
}

BwrapBuilder& BwrapBuilder::AddDir(const std::string& sandbox) {
    if (pending_perms_) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%o", *pending_perms_);
        mounts_.push_back({"--perms", buf});
        pending_perms_.reset();
    }
    mounts_.push_back({"--dir", sandbox});
    return *this;
}

BwrapBuilder& BwrapBuilder::AddDev(const std::string& sandbox) {
    mounts_.push_back({"--dev", sandbox});
    return *this;
}

BwrapBuilder& BwrapBuilder::AddProc(const std::string& sandbox) {
    mounts_.push_back({"--proc", sandbox});
    return *this;
}

BwrapBuilder& BwrapBuilder::AddEnv(const std::string& key, const std::string& value) {
    env_.emplace_back(key, value);
    return *this;
}

BwrapBuilder& BwrapBuilder::SetWorkingDirectory(const std::string& dir) {
    chdir_ = dir;
    return *this;
}

std::vector<std::string> BwrapBuilder::Build(const std::vector<std::string>& command) const {
    std::vector<std::string> args{binary_, "--unshare-all"};
    if (share_net_) args.emplace_back("--share-net");
    args.insert(args.end(), {"--die-with-parent", "--new-session", "--clearenv"});
    for (const auto& m : mounts_) args.insert(args.end(), m.begin(), m.end());
    for (const auto& [k, v] : env_) args.insert(args.end(), {"--setenv", k, v});
    args.insert(args.end(), {"--chdir", chdir_, "--"});
    args.insert(args.end(), command.begin(), command.end());
    return args;
}

} // namespace zeromesh::exec
