#pragma once

#include <optional>
#include <string>
#include <vector>

namespace zeromesh::exec {

// Builds a bubblewrap command line. Always isolates every namespace (network shared only on
// request), clears the environment, starts a new session and dies with its parent.
// Mounts are emitted in the order they were added.
class BwrapBuilder {
public:
    BwrapBuilder& SetBinary(std::string path);
    BwrapBuilder& ShareNetwork(bool share);
    BwrapBuilder& AddReadOnlyBind(const std::string& host, const std::string& sandbox);
    BwrapBuilder& AddReadWriteBind(const std::string& host, const std::string& sandbox);
    BwrapBuilder& AddSymlink(const std::string& target, const std::string& link);
    BwrapBuilder& AddTmpfs(const std::string& sandbox);
    // Applies to the next AddDir (bwrap --perms OCTAL).
    BwrapBuilder& AddPerms(unsigned mode);
    BwrapBuilder& AddDir(const std::string& sandbox);
    BwrapBuilder& AddDev(const std::string& sandbox);
    BwrapBuilder& AddProc(const std::string& sandbox);
    BwrapBuilder& AddEnv(const std::string& key, const std::string& value);
    BwrapBuilder& SetWorkingDirectory(const std::string& dir);

    [[nodiscard]] std::vector<std::string> Build(const std::vector<std::string>& command) const;

private:
    std::string binary_{"bwrap"};
    bool share_net_{false};
    std::vector<std::vector<std::string>> mounts_;
    std::vector<std::pair<std::string, std::string>> env_;
    std::string chdir_{"/"};
    std::optional<unsigned> pending_perms_;
};

} // namespace zeromesh::exec
