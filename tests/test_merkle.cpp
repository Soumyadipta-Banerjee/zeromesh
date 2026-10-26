#include "tests/test_framework.hpp"
#include "cas/merkle.hpp"
#include "cas/store.hpp"

#include <filesystem>
#include <fstream>

TEST(DirectoryManifest_SerializationRoundtrip) {
    using namespace zeromesh::cas;

    DirectoryManifest orig;
    orig.files.push_back(FileNode{
        .name = "compiler.cpp",
        .digest = Digest{"hash_compiler", 1024},
        .is_executable = false
    });
    orig.files.push_back(FileNode{
        .name = "compile.sh",
        .digest = Digest{"hash_script", 256},
        .is_executable = true
    });

    orig.directories.push_back(DirectoryNode{
        .name = "subfolder",
        .digest = Digest{"hash_subfolder_manifest", 512}
    });

    orig.symlinks.push_back(SymlinkNode{
        .name = "liblink.so",
        .target = "libcore.so.1"
    });

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
    using namespace zeromesh::cas;

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
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
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
