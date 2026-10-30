#include "tests/unit/test_framework.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <filesystem>
#include <fstream>

TEST(DirectoryManifest_SerializationRoundtrip) {
    using namespace zeromesh::store;

    DirectoryManifest orig;
    orig.files.push_back(
        FileNode{.name = "compiler.cpp", .digest = Digest{"hash_compiler", 1024}, .is_executable = false});
    orig.files.push_back(FileNode{.name = "compile.sh", .digest = Digest{"hash_script", 256}, .is_executable = true});

    orig.directories.push_back(DirectoryNode{.name = "subfolder", .digest = Digest{"hash_subfolder_manifest", 512}});

    orig.symlinks.push_back(SymlinkNode{.name = "liblink.so", .target = "libcore.so.1"});

    std::string serialized = orig.Serialize();
    ASSERT_FALSE(serialized.empty());

    auto deserialized = DirectoryManifest::Deserialize(serialized);
    ASSERT_TRUE(deserialized.has_value());

    ASSERT_EQ(deserialized->files.size(), 2);
    ASSERT_EQ(deserialized->directories.size(), 1);
    ASSERT_EQ(deserialized->symlinks.size(), 1);

    // Serialization sorts files deterministically by name: compile.sh before compiler.cpp
    ASSERT_EQ(deserialized->files[0].name, "compile.sh");
    ASSERT_EQ(deserialized->files[0].is_executable, true);
    ASSERT_EQ(deserialized->files[0].digest.hash, "hash_script");

    ASSERT_EQ(deserialized->files[1].name, "compiler.cpp");
    ASSERT_EQ(deserialized->files[1].is_executable, false);
    ASSERT_EQ(deserialized->files[1].digest.hash, "hash_compiler");

    ASSERT_EQ(deserialized->directories[0].name, "subfolder");
    ASSERT_EQ(deserialized->directories[0].digest.hash, "hash_subfolder_manifest");

    ASSERT_EQ(deserialized->symlinks[0].name, "liblink.so");
    ASSERT_EQ(deserialized->symlinks[0].target, "libcore.so.1");
}

TEST(MerkleBuilder_BuildAndMaterializeTree) {
    using namespace zeromesh::store;

    auto test_root = std::filesystem::temp_directory_path() / "zm_test_merkle_src";
    auto test_dest = std::filesystem::temp_directory_path() / "zm_test_merkle_dst";
    auto test_cas = std::filesystem::temp_directory_path() / "zm_test_merkle_cas";

    std::filesystem::remove_all(test_root);
    std::filesystem::remove_all(test_dest);
    std::filesystem::remove_all(test_cas);

    // Create source directory tree
    std::filesystem::create_directories(test_root / "src");
    std::filesystem::create_directories(test_root / "bin");

    // Regular file 1
    {
        std::ofstream f(test_root / "src" / "main.cpp");
        f << "#include <iostream>\nint main() { return 0; }\n";
    }

    // Executable file 2
    {
        std::ofstream f(test_root / "bin" / "build.sh");
        f << "#!/bin/bash\necho Building...\n";
    }
    std::filesystem::permissions(test_root / "bin" / "build.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);

    // Symlink
    {
        std::error_code ec;
        std::filesystem::create_symlink("../src/main.cpp", test_root / "bin" / "main_link.cpp", ec);
    }

    auto store = std::make_shared<CasStore>(test_cas);
    MerkleBuilder builder(store);

    // 1. Build Merkle tree from disk
    Digest root_digest = builder.BuildTree(test_root);
    ASSERT_FALSE(root_digest.hash.empty());
    ASSERT_TRUE(root_digest.size_bytes > 0);
    ASSERT_TRUE(store->HasBlob(root_digest.hash));

    // 2. Materialize tree into destination
    ASSERT_TRUE(builder.MaterializeTree(root_digest, test_dest));

    // 3. Verify destination matches source
    ASSERT_TRUE(std::filesystem::exists(test_dest / "src" / "main.cpp"));
    ASSERT_TRUE(std::filesystem::exists(test_dest / "bin" / "build.sh"));
    ASSERT_TRUE(std::filesystem::exists(test_dest / "bin" / "main_link.cpp"));

    // Check executable bit preserved
    auto build_sh_perms = std::filesystem::status(test_dest / "bin" / "build.sh").permissions();
    ASSERT_TRUE((build_sh_perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);

    // 4. Rebuilding tree from materialized dest produces identical Merkle root digest (idempotence)
    Digest dest_root_digest = builder.BuildTree(test_dest);
    ASSERT_EQ(root_digest.hash, dest_root_digest.hash);

    // Cleanup
    std::filesystem::remove_all(test_root);
    std::filesystem::remove_all(test_dest);
    std::filesystem::remove_all(test_cas);
}

#include "tests/unit/test_util.hpp"
#include "store/ignore.hpp"

namespace {
using zeromesh::testing::ReadFile;
using zeromesh::testing::TempDir;
using zeromesh::testing::WriteFile;

struct Stores {
    TempDir dir;
    std::shared_ptr<zeromesh::store::CasStore> cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    zeromesh::store::MerkleBuilder merkle{cas};
};
} // namespace

TEST(MerkleOverlayFile) {
    Stores s;
    TempDir src;
    WriteFile(src / "a.txt", "A");
    auto base = s.merkle.BuildTree(src.path());
    auto png = s.cas->PutBlob("PNG");
    auto merged = s.merkle.Overlay(base, "frames/f1.png", png, false);
    TempDir out;
    ASSERT_TRUE(s.merkle.MaterializeTree(merged, out / "m"));
    ASSERT_EQ(ReadFile(out / "m/a.txt"), std::string("A"));
    ASSERT_EQ(ReadFile(out / "m/frames/f1.png"), std::string("PNG"));
}

TEST(MerkleOverlayPreservesExecutable) {
    Stores s;
    auto blob = s.cas->PutBlob("#!/bin/sh\necho hi\n");
    zeromesh::store::Digest empty_root;
    auto merged = s.merkle.Overlay(empty_root, "bin/tool", blob, false, true);
    TempDir out;
    ASSERT_TRUE(s.merkle.MaterializeTree(merged, out / "m"));
    const auto perms = std::filesystem::status(out / "m/bin/tool").permissions();
    ASSERT_NE((perms & std::filesystem::perms::owner_exec), std::filesystem::perms::none);
}

TEST(MerkleOverlayTreeMerges) {
    Stores s;
    TempDir base_src, item_src;
    WriteFile(base_src / "frames/old.png", "OLD");
    WriteFile(item_src / "new.png", "NEW");
    auto base = s.merkle.BuildTree(base_src.path());
    auto item = s.merkle.BuildTree(item_src.path());
    auto merged = s.merkle.Overlay(base, "frames", item, true);
    TempDir out;
    ASSERT_TRUE(s.merkle.MaterializeTree(merged, out / "m"));
    ASSERT_EQ(ReadFile(out / "m/frames/old.png"), std::string("OLD"));
    ASSERT_EQ(ReadFile(out / "m/frames/new.png"), std::string("NEW"));
}

TEST(MerkleSymlinkNotFollowed) {
    Stores s;
    TempDir src;
    std::filesystem::create_symlink("/etc/passwd", src / "link");
    auto root = s.merkle.BuildTree(src.path());
    auto manifest = zeromesh::store::DirectoryManifest::Deserialize(*s.cas->GetBlob(root.hash));
    ASSERT_EQ(manifest->symlinks.size(), static_cast<size_t>(1));
    ASSERT_EQ(manifest->files.size(), static_cast<size_t>(0));
    TempDir out;
    ASSERT_TRUE(s.merkle.MaterializeTree(root, out / "m"));
    ASSERT_TRUE(std::filesystem::is_symlink(out / "m/link"));
    ASSERT_EQ(std::filesystem::read_symlink(out / "m/link").string(), std::string("/etc/passwd"));
}

TEST(MerkleRejectsTraversal) {
    Stores s;
    auto blob = s.cas->PutBlob("evil");
    zeromesh::store::DirectoryManifest m;
    m.files.push_back({.name = "../evil", .digest = blob, .is_executable = false});
    auto root = s.cas->PutBlob(m.Serialize());
    TempDir out;
    ASSERT_FALSE(s.merkle.MaterializeTree(root, out / "m"));
    ASSERT_FALSE(std::filesystem::exists(out / "evil"));
}

TEST(MerkleUnicodeSpaces) {
    Stores s;
    TempDir src;
    WriteFile(src / "dir with space/ünï.txt", "u");
    auto root = s.merkle.BuildTree(src.path());
    TempDir out;
    ASSERT_TRUE(s.merkle.MaterializeTree(root, out / "m"));
    ASSERT_EQ(ReadFile(out / "m/dir with space/ünï.txt"), std::string("u"));
}

TEST(MerkleBuildHonoursIgnoreAndCountsBytes) {
    Stores s;
    TempDir src;
    WriteFile(src / "keep.txt", "12345");
    WriteFile(src / "skip.log", "xxxxxxxx");
    WriteFile(src / ".git/HEAD", "ref");
    WriteFile(src / ".zmignore", "*.log\n");
    uint64_t total = 0;
    auto root = s.merkle.BuildTree(src.path(), zeromesh::store::IgnoreRules::ForRoot(src.path()), &total);
    TempDir out;
    ASSERT_TRUE(s.merkle.MaterializeTree(root, out / "m"));
    ASSERT_TRUE(std::filesystem::exists(out / "m/keep.txt"));
    ASSERT_FALSE(std::filesystem::exists(out / "m/skip.log"));
    ASSERT_FALSE(std::filesystem::exists(out / "m/.git"));
    ASSERT_EQ(total, static_cast<uint64_t>(5 + 6)); // keep.txt + .zmignore ("*.log\n")
}
