#include "tests/unit/test_framework.hpp"
#include "cas/merkle.hpp"
#include "cas/store.hpp"
#include "executor/landlock.hpp"
#include "executor/runner.hpp"

#include <filesystem>
#include <fstream>

TEST(LandlockSandbox_SupportCheck) {
    using namespace zeromesh::executor;
    // On modern Linux kernels (>= 5.13), Landlock must be supported
    ASSERT_TRUE(LandlockSandbox::IsSupported());
}

TEST(TaskRunner_SimpleEchoExecution) {
    using namespace zeromesh::cas;
    using namespace zeromesh::executor;

    auto test_cas = std::filesystem::temp_directory_path() / "zm_test_runner_echo_cas";
    std::filesystem::remove_all(test_cas);

    auto store = std::make_shared<CasStore>(test_cas);
    auto merkle = std::make_shared<MerkleBuilder>(store);
    TaskRunner runner(store, merkle);

    ExecutionRequest req{
        .action_id = "test-echo-action-001",
        .command_args = {"echo", "ZeroMesh Remote Execution Works!"},
        .env_vars = {},
        .input_root_digest = Digest{"", 0},
        .expected_output_files = {},
        .expected_output_directories = {},
        .working_directory = "."
    };

    auto res = runner.Run(req);
    ASSERT_EQ(res.exit_code, 0);
    ASSERT_TRUE(res.stdout_raw.find("ZeroMesh Remote Execution Works!") != std::string::npos);

    // Verify stdout was captured as a blob in CAS
    ASSERT_FALSE(res.stdout_digest.hash.empty());
    auto stored_stdout = store->GetBlob(res.stdout_digest.hash);
    ASSERT_TRUE(stored_stdout.has_value());
    ASSERT_EQ(*stored_stdout, res.stdout_raw);

    std::filesystem::remove_all(test_cas);
}

TEST(TaskRunner_OutputArtifactHarvesting) {
    using namespace zeromesh::cas;
    using namespace zeromesh::executor;

    auto test_cas = std::filesystem::temp_directory_path() / "zm_test_runner_harvest_cas";
    std::filesystem::remove_all(test_cas);

    auto store = std::make_shared<CasStore>(test_cas);
    auto merkle = std::make_shared<MerkleBuilder>(store);
    TaskRunner runner(store, merkle);

    ExecutionRequest req{
        .action_id = "test-harvest-action-002",
        .command_args = {"sh", "-c", "echo 'compiled binary data' > binary.o"},
        .env_vars = {},
        .input_root_digest = Digest{"", 0},
        .expected_output_files = {"binary.o"},
        .expected_output_directories = {},
        .working_directory = "."
    };

    auto res = runner.Run(req);
    ASSERT_EQ(res.exit_code, 0);
    ASSERT_EQ(res.output_files.size(), 1);
    ASSERT_EQ(res.output_files[0].first, "binary.o");

    // Verify harvested output artifact in CAS
    auto harvested_blob = store->GetBlob(res.output_files[0].second.hash);
    ASSERT_TRUE(harvested_blob.has_value());
    ASSERT_EQ(*harvested_blob, "compiled binary data\n");

    std::filesystem::remove_all(test_cas);
}

TEST(TaskRunner_FailureExitCodeAndStderrCapture) {
    using namespace zeromesh::cas;
    using namespace zeromesh::executor;

    auto test_cas = std::filesystem::temp_directory_path() / "zm_test_runner_fail_cas";
    std::filesystem::remove_all(test_cas);

    auto store = std::make_shared<CasStore>(test_cas);
    auto merkle = std::make_shared<MerkleBuilder>(store);
    TaskRunner runner(store, merkle);

    ExecutionRequest req{
        .action_id = "test-fail-action-003",
        .command_args = {"sh", "-c", "echo 'compilation failed: syntax error' >&2; exit 42"},
        .env_vars = {},
        .input_root_digest = Digest{"", 0},
        .expected_output_files = {},
        .expected_output_directories = {},
        .working_directory = "."
    };

    auto res = runner.Run(req);
    ASSERT_EQ(res.exit_code, 42);
    ASSERT_TRUE(res.stderr_raw.find("compilation failed: syntax error") != std::string::npos);

    // Verify stderr was stored in CAS
    auto stored_stderr = store->GetBlob(res.stderr_digest.hash);
    ASSERT_TRUE(stored_stderr.has_value());
    ASSERT_EQ(*stored_stderr, res.stderr_raw);

    std::filesystem::remove_all(test_cas);
}

TEST(TaskRunner_InputTreeMaterializationAndCompile) {
    using namespace zeromesh::cas;
    using namespace zeromesh::executor;

    auto test_cas = std::filesystem::temp_directory_path() / "zm_test_runner_compile_cas";
    auto test_src = std::filesystem::temp_directory_path() / "zm_test_runner_compile_src";

    std::filesystem::remove_all(test_cas);
    std::filesystem::remove_all(test_src);
    std::filesystem::create_directories(test_src);

    // Write a small C program to test_src
    {
        std::ofstream f(test_src / "test.c");
        f << "#include <stdio.h>\nint main() { printf(\"HELLO_ZEROMESH\\n\"); return 0; }\n";
    }

    auto store = std::make_shared<CasStore>(test_cas);
    auto merkle = std::make_shared<MerkleBuilder>(store);

    // Build Merkle tree of source
    Digest input_root = merkle->BuildTree(test_src);
    ASSERT_FALSE(input_root.hash.empty());

    TaskRunner runner(store, merkle);

    ExecutionRequest req{
        .action_id = "test-compile-action-004",
        .command_args = {"gcc", "-O2", "test.c", "-o", "test_app"},
        .env_vars = {},
        .input_root_digest = input_root,
        .expected_output_files = {"test_app"},
        .expected_output_directories = {},
        .working_directory = "."
    };

    auto res = runner.Run(req);
    ASSERT_EQ(res.exit_code, 0);
    ASSERT_EQ(res.output_files.size(), 1);
    ASSERT_EQ(res.output_files[0].first, "test_app");

    // Verify compiled binary was harvested into CAS
    auto binary_blob = store->GetBlob(res.output_files[0].second.hash);
    ASSERT_TRUE(binary_blob.has_value());
    ASSERT_TRUE(binary_blob->size() > 0);

    std::filesystem::remove_all(test_cas);
    std::filesystem::remove_all(test_src);
}
