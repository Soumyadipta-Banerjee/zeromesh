#include "exec/bwrap.hpp"

namespace zeromesh::exec {

BwrapBuilder& BwrapBuilder::SetBinary(std::string path) { binary_ = std::move(path); return *this; }
BwrapBuilder& BwrapBuilder::ShareNetwork(bool share) { share_net_ = share; return *this; }

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

BwrapBuilder& BwrapBuilder::SetWorkingDirectory(const std::string& dir) { chdir_ = dir; return *this; }

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
