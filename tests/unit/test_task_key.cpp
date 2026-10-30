#include "tests/unit/test_framework.hpp"
#include "node/task_key.hpp"

namespace {

zeromesh::v1::TaskSpec Echo(const std::string& word) {
    zeromesh::v1::TaskSpec s;
    s.add_argv("echo");
    s.add_argv(word);
    s.mutable_env_spec()->set_name("host");
    return s;
}

} // namespace

TEST(TaskKeyStable) {
    auto a = zeromesh::node::TaskKey(Echo("hi"));
    auto b = zeromesh::node::TaskKey(Echo("hi"));
    ASSERT_EQ(a, b);
    ASSERT_EQ(a.size(), static_cast<size_t>(64));
}

TEST(TaskKeyIgnoresName) {
    auto a = Echo("hi");
    auto b = Echo("hi");
    a.set_name("a");
    b.set_name("b");
    ASSERT_EQ(zeromesh::node::TaskKey(a), zeromesh::node::TaskKey(b));
}

TEST(TaskKeyArgvMatters) {
    ASSERT_NE(zeromesh::node::TaskKey(Echo("hi")), zeromesh::node::TaskKey(Echo("ho")));
}

TEST(TaskKeyArgvBoundariesMatter) {
    zeromesh::v1::TaskSpec a, b;
    a.add_argv("ab");
    a.add_argv("c");
    b.add_argv("a");
    b.add_argv("bc");
    ASSERT_NE(zeromesh::node::TaskKey(a), zeromesh::node::TaskKey(b));
}

TEST(TaskKeyEnvOrderIrrelevant) {
    auto a = Echo("hi");
    auto b = Echo("hi");
    (*a.mutable_env())["X"] = "1";
    (*a.mutable_env())["Y"] = "2";
    (*b.mutable_env())["Y"] = "2";
    (*b.mutable_env())["X"] = "1";
    ASSERT_EQ(zeromesh::node::TaskKey(a), zeromesh::node::TaskKey(b));
}

TEST(TaskKeyInputRootMatters) {
    auto a = Echo("hi");
    auto b = Echo("hi");
    a.mutable_input_root()->set_hash(std::string(64, 'a'));
    b.mutable_input_root()->set_hash(std::string(64, 'b'));
    ASSERT_NE(zeromesh::node::TaskKey(a), zeromesh::node::TaskKey(b));
}

TEST(TaskKeyToolchainDigestMatters) {
    auto a = Echo("hi");
    auto b = Echo("hi");
    a.set_toolchain_digest("gcc-15");
    b.set_toolchain_digest("gcc-16");
    ASSERT_NE(zeromesh::node::TaskKey(a), zeromesh::node::TaskKey(b));
    ASSERT_NE(zeromesh::node::TaskKey(a), zeromesh::node::TaskKey(Echo("hi")));
}

TEST(TaskKeyIgnoresRetriesAndPlacement) {
    auto a = Echo("hi");
    auto b = Echo("hi");
    b.mutable_policy()->set_max_retries(9);
    b.mutable_policy()->set_may_run_local(false);
    b.mutable_resources()->set_cores(8);
    ASSERT_EQ(zeromesh::node::TaskKey(a), zeromesh::node::TaskKey(b));
}

TEST(NormalizeDefaults) {
    zeromesh::v1::TaskSpec s;
    zeromesh::node::Normalize(s);
    ASSERT_EQ(s.policy().max_retries(), 3);
    ASSERT_EQ(s.policy().timeout_s(), 3600);
    ASSERT_TRUE(s.policy().may_run_local());
    ASSERT_TRUE(s.policy().may_run_remote());
    ASSERT_FALSE(s.policy().allow_network());
    ASSERT_TRUE(s.policy().preemptible());
    ASSERT_EQ(s.working_dir(), std::string("."));
    ASSERT_EQ(s.env_spec().name(), std::string("host"));
}

TEST(NormalizeKeepsExplicitValues) {
    zeromesh::v1::TaskSpec s;
    s.mutable_policy()->set_may_run_local(false);
    s.mutable_policy()->set_max_retries(0);
    zeromesh::node::Normalize(s);
    ASSERT_FALSE(s.policy().may_run_local());
    ASSERT_EQ(s.policy().max_retries(), 0);
    ASSERT_EQ(s.policy().timeout_s(), 3600);
}
