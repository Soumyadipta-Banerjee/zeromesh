#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "node/state.hpp"

#include <sys/stat.h>

using zeromesh::node::NodeState;
using zeromesh::testing::TempDir;

TEST(StatePersistsId) {
    TempDir tmp;
    const auto dir = tmp / "state"; // does not exist yet
    auto a = NodeState::LoadOrCreate(dir);
    auto b = NodeState::LoadOrCreate(dir);
    ASSERT_EQ(a.node_id().size(), 32u);
    ASSERT_EQ(a.node_id(), b.node_id());
    ASSERT_EQ(a.dir(), dir);

    struct stat st{};
    ASSERT_EQ(::stat((dir / "node_id").c_str(), &st), 0);
    ASSERT_EQ(st.st_mode & 0777, 0600u);
    ASSERT_EQ(::stat(dir.c_str(), &st), 0);
    ASSERT_EQ(st.st_mode & 0777, 0700u);
}

TEST(StateDistinctDirs) {
    TempDir a;
    TempDir b;
    ASSERT_NE(NodeState::LoadOrCreate(a.path()).node_id(), NodeState::LoadOrCreate(b.path()).node_id());
}
