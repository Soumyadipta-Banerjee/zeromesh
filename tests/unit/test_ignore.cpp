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

TEST(IgnoreExtraPatternsComeBeforeTheRootsFile) {
    TempDir dir;
    WriteFile(dir / ".zmignore", "!keep.o\n");
    auto r = IgnoreRules::ForRoot(dir.path(), {"*.o", "build/"});
    ASSERT_TRUE(r.Ignored("src/a.o", false));
    ASSERT_TRUE(r.Ignored("build", true));
    ASSERT_FALSE(r.Ignored("src/a.c", false));
    ASSERT_FALSE(r.Ignored("keep.o", false)); // the user's file re-includes it
}

TEST(IgnoreExactRulesKeepTheLastMatchWins) {
    // Rules without a wildcard are looked up rather than tried in turn; the order still decides.
    auto r = IgnoreRules::FromLines({"/src/gen.c", "!src/*.c", "*.o", "!/src/keep.o", "!/lib/keep.o", "*.o"});
    ASSERT_FALSE(r.Ignored("src/gen.c", false)); // a later pattern re-includes an exact path
    ASSERT_TRUE(r.Ignored("src/keep.o", false)); // ...and a later pattern excludes an exact negation again
    r = IgnoreRules::FromLines({"*.o", "!/src/keep.o"});
    ASSERT_FALSE(r.Ignored("src/keep.o", false));
    ASSERT_TRUE(r.Ignored("src/other.o", false));
    r = IgnoreRules::FromLines({"/a/keep.txt", "!keep.txt"});
    ASSERT_FALSE(r.Ignored("a/keep.txt", false));
    r = IgnoreRules::FromLines({"!keep.txt", "/a/keep.txt"});
    ASSERT_TRUE(r.Ignored("a/keep.txt", false));
}

TEST(IgnoreExactRulesHonourDirOnlyAnchoringAndEscapes) {
    auto r = IgnoreRules::FromLines({"/build/", "Makefile.local", "a\\*b"});
    ASSERT_TRUE(r.Ignored("build", true));
    ASSERT_FALSE(r.Ignored("build", false));
    ASSERT_FALSE(r.Ignored("x/build", true));
    ASSERT_TRUE(r.Ignored("x/y/Makefile.local", false));
    ASSERT_FALSE(r.Ignored("Makefile.localx", false));
    ASSERT_TRUE(r.Ignored("a*b", false)); // an escaped "*" is a literal star, as fnmatch reads it
    ASSERT_FALSE(r.Ignored("axb", false));
}

TEST(IgnoredEntryMatchesOnlyTheEntryItself) {
    // For a walk that does not enter ignored directories: their contents are never asked about.
    auto r = IgnoreRules::FromLines({"build/", "*.log"});
    ASSERT_TRUE(r.IgnoredEntry("build", true));
    ASSERT_TRUE(r.IgnoredEntry("src/a.log", false));
    ASSERT_FALSE(r.IgnoredEntry("build/x.o", false));
    ASSERT_TRUE(r.Ignored("build/x.o", false));
    ASSERT_FALSE(r.IgnoredEntry("src/a.c", false));
}
