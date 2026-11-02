#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
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

    auto root_digest = builder.BuildTree(export_dir.path());

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
    auto root_digest = builder.BuildTree(export_dir.path());

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

    auto root_digest = builder.BuildTree(src_dir.path());

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
    std::string read_content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

TEST(EnvMirrorCacheEvictionSkipsPinnedChunks) {
    TempDir cache_dir;
    MirrorCache cache(cache_dir.path(), 100);

    cache.Put("hash1", std::string(50, 'a'));
    cache.Pin("hash1"); // oldest entry, but a running task has it open
    cache.Put("hash2", std::string(40, 'b'));
    cache.Put("hash3", std::string(30, 'c'));

    ASSERT_TRUE(cache.Contains("hash1"));
    ASSERT_FALSE(cache.Contains("hash2"));
    ASSERT_TRUE(cache.Contains("hash3"));

    cache.Unpin("hash1");
    cache.Put("hash4", std::string(50, 'd'));
    ASSERT_FALSE(cache.Contains("hash1"));
    ASSERT_TRUE(cache.Contains("hash4"));
}

TEST(EnvMirrorCachePinsAreReferenceCounted) {
    TempDir cache_dir;
    MirrorCache cache(cache_dir.path(), 100);

    cache.Put("hash1", std::string(60, 'a'));
    cache.Pin("hash1");
    cache.Pin("hash1");
    cache.Unpin("hash1"); // still held by the second opener
    cache.Put("hash2", std::string(60, 'b'));
    ASSERT_TRUE(cache.Contains("hash1"));

    cache.Unpin("hash1");
    cache.Put("hash3", std::string(60, 'c'));
    ASSERT_FALSE(cache.Contains("hash1"));
}

TEST(EnvMirrorCacheExceedsBudgetWhenEverythingPinned) {
    TempDir cache_dir;
    MirrorCache cache(cache_dir.path(), 100);

    cache.Put("hash1", std::string(80, 'a'));
    cache.Pin("hash1");
    cache.Put("hash2", std::string(80, 'b')); // nothing evictable: overshoot rather than drop a live chunk

    ASSERT_TRUE(cache.Contains("hash1"));
    ASSERT_TRUE(cache.Contains("hash2"));
    ASSERT_EQ(cache.CurrentBytes(), 160u);
}
