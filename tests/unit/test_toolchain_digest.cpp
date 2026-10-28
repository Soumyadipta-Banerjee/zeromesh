#include "tests/unit/test_framework.hpp"

#include "exec/toolchain_digest.hpp"

#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;

// An executable script stands in for a tool; its digest covers the script bytes and its interpreter.
fs::path WriteScript(const fs::path& dir, const std::string& name, const std::string& body) {
    fs::create_directories(dir);
    const fs::path path = dir / name;
    std::ofstream(path) << body;
    fs::permissions(path, fs::perms::owner_all);
    return path;
}

} // namespace

TEST(ToolchainDigestStableForSameBinary) {
    const std::string a = zeromesh::exec::ToolchainDigest("/bin/sh", "");
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a, zeromesh::exec::ToolchainDigest("/bin/sh", ""));
}

TEST(ToolchainDigestDiffersAcrossBinaries) {
    ASSERT_NE(zeromesh::exec::ToolchainDigest("/bin/sh", ""), zeromesh::exec::ToolchainDigest("/bin/true", ""));
}

TEST(ToolchainDigestFollowsFileContent) {
    const fs::path dir = fs::temp_directory_path() / "zm-toolchain-digest-test";
    const fs::path tool = WriteScript(dir, "tool", "#!/bin/sh\necho one\n");
    const std::string before = zeromesh::exec::ToolchainDigest(tool.string(), "");
    WriteScript(dir, "tool", "#!/bin/sh\necho two, longer\n");
    const std::string after = zeromesh::exec::ToolchainDigest(tool.string(), "");
    fs::remove_all(dir);
    ASSERT_FALSE(before.empty());
    ASSERT_NE(before, after);
}

TEST(ToolchainDigestResolvesBareNameThroughPath) {
    ASSERT_EQ(zeromesh::exec::ToolchainDigest("sh", "/bin"), zeromesh::exec::ToolchainDigest("/bin/sh", ""));
}

TEST(ToolchainDigestEmptyWhenUnresolvable) {
    ASSERT_TRUE(zeromesh::exec::ToolchainDigest("zm-no-such-tool-anywhere", "/bin:/usr/bin").empty());
    ASSERT_TRUE(zeromesh::exec::ToolchainDigest("", "/bin").empty());
}
