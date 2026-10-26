#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "plugins/env-mirror/deny_list.hpp"
#include "plugins/env-mirror/chunk_source.hpp"
#include "plugins/env-mirror/mirror_cache.hpp"
#include "plugins/env-mirror/mirror_fuse.hpp"
#include "plugins/env-mirror/env_mirror.hpp"
#include "node/requester_blob_source.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <string>

using namespace zeromesh;
using namespace zeromesh::mirror;
using testing::TempDir;
using testing::WriteFile;

TEST(EnvMirrorDenyListExcludesSecrets) {
    // Acceptance criterion 1:
    // "a manifest built from a fake home with .ssh/id_ed25519, project/.env and project/main.c contains only main.c"
    TempDir fake_home;
    WriteFile(fake_home / ".ssh" / "id_ed25519", "PRIVATE KEY DATA");
    WriteFile(fake_home / "project" / ".env", "SECRET=12345");
    WriteFile(fake_home / "project" / "main.c", "int main() { return 0; }\n");

    TempDir cas_dir;
    auto cas = std::make_shared<store::CasStore>(cas_dir.path());
    store::MerkleBuilder builder(cas);

    auto rules = DenyList::CreateIgnoreRules();
    uint64_t total_bytes = 0;
    auto root_digest = builder.BuildTree(fake_home.path(), rules, &total_bytes);

    ASSERT_FALSE(root_digest.hash.empty());

    // Deserialize root manifest
    auto root_data = cas->GetBlob(root_digest.hash);
    ASSERT_TRUE(root_data.has_value());
    auto root_manifest = store::DirectoryManifest::Deserialize(*root_data);
    ASSERT_TRUE(root_manifest.has_value());

    // Root should only contain "project" directory, NOT ".ssh"
    ASSERT_EQ(root_manifest->directories.size(), 1u);
    ASSERT_EQ(root_manifest->directories[0].name, "project");
    ASSERT_EQ(root_manifest->files.size(), 0u);

    // Deserialize project directory manifest
    auto proj_data = cas->GetBlob(root_manifest->directories[0].digest.hash);
    ASSERT_TRUE(proj_data.has_value());
    auto proj_manifest = store::DirectoryManifest::Deserialize(*proj_data);
    ASSERT_TRUE(proj_manifest.has_value());

    // Project should only contain "main.c", NOT ".env"
    ASSERT_EQ(proj_manifest->files.size(), 1u);
    ASSERT_EQ(proj_manifest->files[0].name, "main.c");
    ASSERT_EQ(proj_manifest->directories.size(), 0u);
}

TEST(EnvMirrorMountReadOnlyFailsWithEROFS) {
    // Acceptance criterion 2:
    // "A write through the mount fails with EROFS"
    if (!MirrorFuse::IsFuseAvailable()) {
        std::cout << "[SKIP] FUSE not available (/dev/fuse not accessible)\n";
        return;
    }

    TempDir scratch;
    TempDir cas_dir;
    auto cas = std::make_shared<store::CasStore>(cas_dir.path());
    store::MerkleBuilder builder(cas);

    TempDir export_dir;
    WriteFile(export_dir / "project" / "main.c", "int main() { return 0; }\n");

    auto rules = DenyList::CreateIgnoreRules();
    uint64_t total_bytes = 0;
    auto root_digest = builder.BuildTree(export_dir.path(), rules, &total_bytes);

    auto env = MakeMirrorEnv();
    node::LocalCasBlobSource blob_source(cas);

    plugin::TaskContext ctx;
    ctx.task_id = "task-erofs-test";
    ctx.scratch_dir = scratch.path();
    ctx.env.params["root_digest"] = root_digest.hash;
    ctx.blob_source = &blob_source;

    auto prep = env->Prepare(ctx);
    ASSERT_TRUE(prep.value.has_value());

    auto mount_path = scratch.path() / "mirror";
    ASSERT_TRUE(MirrorFuse::IsPathMounted(mount_path));

    // Try creating a new file through the mount
    int fd = ::open((mount_path / "new_file.txt").c_str(), O_WRONLY | O_CREAT, 0644);
    ASSERT_EQ(fd, -1);
    ASSERT_EQ(errno, EROFS);

    // Try modifying an existing file
    int fd_exist = ::open((mount_path / "project" / "main.c").c_str(), O_WRONLY);
    ASSERT_EQ(fd_exist, -1);
    ASSERT_EQ(errno, EROFS);

    // Release cleanly
    env->Release(ctx);
    ASSERT_FALSE(MirrorFuse::IsPathMounted(mount_path));
}

TEST(EnvMirrorReleaseLeavesNoMountBehind) {
    // Acceptance criterion 3:
    // "Release leaves no mount behind (check /proc/self/mounts)"
    if (!MirrorFuse::IsFuseAvailable()) {
        std::cout << "[SKIP] FUSE not available (/dev/fuse not accessible)\n";
        return;
    }

    TempDir scratch;
    TempDir cas_dir;
    auto cas = std::make_shared<store::CasStore>(cas_dir.path());
    store::MerkleBuilder builder(cas);
    TempDir export_dir;
    WriteFile(export_dir / "x", "x\n");
    uint64_t total_bytes = 0;
    auto root_digest = builder.BuildTree(export_dir.path(), store::IgnoreRules::None(), &total_bytes);

    auto env = MakeMirrorEnv();
    node::LocalCasBlobSource blob_source(cas);

    plugin::TaskContext ctx;
    ctx.task_id = "task-release-test";
    ctx.scratch_dir = scratch.path();
    ctx.env.params["root_digest"] = root_digest.hash;
    ctx.blob_source = &blob_source;

    auto prep = env->Prepare(ctx);
    ASSERT_TRUE(prep.value.has_value());

    auto mount_path = scratch.path() / "mirror";
    ASSERT_TRUE(MirrorFuse::IsPathMounted(mount_path));

    env->Release(ctx);

    // Check /proc/self/mounts explicitly
    ASSERT_FALSE(MirrorFuse::IsPathMounted(mount_path));
}

TEST(EnvMirrorReadContentThroughMount) {
    if (!MirrorFuse::IsFuseAvailable()) {
        std::cout << "[SKIP] FUSE not available (/dev/fuse not accessible)\n";
        return;
    }

    TempDir scratch;
    TempDir cas_dir;
    auto cas = std::make_shared<store::CasStore>(cas_dir.path());
    store::MerkleBuilder builder(cas);

    TempDir src_dir;
    const std::string content = "printf(\"hello mirror!\\n\");\n";
    WriteFile(src_dir / "code.c", content);

    uint64_t total_bytes = 0;
    auto root_digest = builder.BuildTree(src_dir.path(), store::IgnoreRules::None(), &total_bytes);

    auto env = MakeMirrorEnv();
    node::LocalCasBlobSource blob_source(cas);

    plugin::TaskContext ctx;
    ctx.task_id = "task-read-test";
    ctx.scratch_dir = scratch.path();
    ctx.env.params["root_digest"] = root_digest.hash;
    ctx.blob_source = &blob_source;

    auto prep = env->Prepare(ctx);
    ASSERT_TRUE(prep.value.has_value());

    auto mount_path = scratch.path() / "mirror";
    std::ifstream in(mount_path / "code.c");
    ASSERT_TRUE(in.is_open());
    std::string read_content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    ASSERT_EQ(read_content, content);

    env->Release(ctx);
    ASSERT_FALSE(MirrorFuse::IsPathMounted(mount_path));
}

TEST(EnvMirrorCheckMethod) {
    auto env = MakeMirrorEnv();
    ASSERT_EQ(env->Name(), std::string_view("mirror"));

    plugin::EnvParams empty_argv;
    ASSERT_FALSE(env->Check(empty_argv).ok);

    plugin::EnvParams ok_params;
    ok_params.argv = {"ls"};
    ok_params.params["root_digest"] = "a" + std::string(63, 'b');
    ASSERT_TRUE(env->Check(ok_params).ok);

    plugin::EnvParams unreachable;
    unreachable.argv = {"ls"};
    unreachable.params["requester_unreachable"] = "true";
    ASSERT_FALSE(env->Check(unreachable).ok);
}

TEST(EnvMirrorDenyListCoversZeromeshStateAndConfig) {
    auto rules = DenyList::CreateIgnoreRules();
    ASSERT_TRUE(rules.Ignored(".local/share/zeromesh", true));
    ASSERT_TRUE(rules.Ignored(".local/share/zeromesh/node.key", false));
    ASSERT_TRUE(rules.Ignored(".local/share/zeromesh/team", false));
    ASSERT_TRUE(rules.Ignored(".config/zeromesh", true));
    ASSERT_TRUE(rules.Ignored(".config/zeromesh/donor.toml", false));
}

TEST(EnvMirrorCustomDenyList) {
    TempDir config_dir;
    WriteFile(config_dir / "mirror_deny", "*.custom_secret\nsecrets/\n");

    auto rules = DenyList::CreateIgnoreRules(config_dir / "mirror_deny");
    ASSERT_TRUE(rules.Ignored(".ssh", true));
    ASSERT_TRUE(rules.Ignored("pass.custom_secret", false));
    ASSERT_TRUE(rules.Ignored("secrets", true));
    ASSERT_FALSE(rules.Ignored("hello.txt", false));
}

TEST(EnvMirrorCacheLruEviction) {
    TempDir cache_dir;
    // Cache capacity = 100 bytes
    MirrorCache cache(cache_dir.path(), 100);

    std::string data50(50, 'a');
    std::string data40(40, 'b');
    std::string data30(30, 'c');

    cache.Put("hash1", data50);
    ASSERT_EQ(cache.CurrentBytes(), 50u);
    ASSERT_TRUE(cache.Contains("hash1"));

    cache.Put("hash2", data40);
    ASSERT_EQ(cache.CurrentBytes(), 90u);
    ASSERT_TRUE(cache.Contains("hash2"));

    // Inserting data30 causes total to exceed 100 (90 + 30 = 120), evicting hash1
    cache.Put("hash3", data30);
    ASSERT_FALSE(cache.Contains("hash1"));
    ASSERT_TRUE(cache.Contains("hash2"));
    ASSERT_TRUE(cache.Contains("hash3"));
    ASSERT_EQ(cache.EvictionCount(), 1u);
}
