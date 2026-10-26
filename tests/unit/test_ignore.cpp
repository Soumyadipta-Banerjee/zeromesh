#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "store/ignore.hpp"

using zeromesh::store::IgnoreRules;
using zeromesh::testing::TempDir;
using zeromesh::testing::WriteFile;

TEST(IgnoreGitAlways) {
    TempDir d;
    auto r = IgnoreRules::ForRoot(d.path());
    ASSERT_TRUE(r.Ignored(".git", true));
    ASSERT_TRUE(r.Ignored(".git/config", false));
    ASSERT_FALSE(r.Ignored("src/main.c", false));
}

TEST(IgnoreDirOnly) {
    TempDir d;
    WriteFile(d / ".zmignore", "build/\n");
    auto r = IgnoreRules::ForRoot(d.path());
    ASSERT_TRUE(r.Ignored("build", true));
    ASSERT_FALSE(r.Ignored("build", false));
}

TEST(IgnoreNegation) {
    TempDir d;
    WriteFile(d / ".zmignore", "# logs\n*.log\n!keep.log\n");
    auto r = IgnoreRules::ForRoot(d.path());
    ASSERT_TRUE(r.Ignored("a.log", false));
    ASSERT_TRUE(r.Ignored("sub/b.log", false));
    ASSERT_FALSE(r.Ignored("keep.log", false));
}

TEST(IgnoreDoubleStar) {
    TempDir d;
    WriteFile(d / ".zmignore", "**/tmp\n");
    auto r = IgnoreRules::ForRoot(d.path());
    ASSERT_TRUE(r.Ignored("a/b/tmp", true));
    ASSERT_TRUE(r.Ignored("tmp", true));
    ASSERT_FALSE(r.Ignored("a/tmpx", true));
}

TEST(IgnoreAnchored) {
    TempDir d;
    WriteFile(d / ".zmignore", "/out\n");
    auto r = IgnoreRules::ForRoot(d.path());
    ASSERT_TRUE(r.Ignored("out", true));
    ASSERT_FALSE(r.Ignored("x/out", true));
}

TEST(IgnoreQuestionMark) {
    TempDir d;
    WriteFile(d / ".zmignore", "f?.txt\n");
    auto r = IgnoreRules::ForRoot(d.path());
    ASSERT_TRUE(r.Ignored("f1.txt", false));
    ASSERT_FALSE(r.Ignored("f12.txt", false));
}
