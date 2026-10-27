#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "store/deny_list.hpp"
#include "store/merkle.hpp"
#include "store/stat_cache.hpp"
#include "store/store.hpp"

#include <sys/stat.h>
#include <utime.h>

#include <chrono>
#include <filesystem>

using zeromesh::testing::TempDir;
using zeromesh::testing::WriteFile;
namespace store = zeromesh::store;

namespace {

// Moves a file's mtime into the past, so the stat cache trusts it (it never trusts a file
// modified within a second of being hashed).
void Age(const std::filesystem::path& p) {
    struct stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    struct utimbuf t{};
    t.actime = st.st_atime - 10;
    t.modtime = st.st_mtime - 10;
    ASSERT_EQ(::utime(p.c_str(), &t), 0);
}

} // namespace

TEST(StatCacheReusesDigestOfUnchangedFile) {
    TempDir d;
    WriteFile(d / "src/a.txt", "alpha");
    Age(d / "src/a.txt");
    auto cas = std::make_shared<store::CasStore>(d / "cas");
    store::MerkleBuilder merkle(cas);

    store::Digest first;
    {
        store::StatCache cache(d / "statcache");
        store::TreeOptions opts;
        opts.stat_cache = &cache;
        first = merkle.BuildTree(d / "src", opts);
        ASSERT_EQ(cache.misses(), 1u);
        cache.Save();
    }
    store::StatCache cache(d / "statcache");
    store::TreeOptions opts;
    opts.stat_cache = &cache;
    ASSERT_EQ(merkle.BuildTree(d / "src", opts).hash, first.hash);
    ASSERT_EQ(cache.hits(), 1u);
    ASSERT_EQ(cache.misses(), 0u);
}

TEST(StatCacheRehashesChangedAndRecentFiles) {
    TempDir d;
    WriteFile(d / "src/a.txt", "alpha");
    Age(d / "src/a.txt");
    auto cas = std::make_shared<store::CasStore>(d / "cas");
    store::MerkleBuilder merkle(cas);
    {
        store::StatCache cache(d / "statcache");
        store::TreeOptions opts;
        opts.stat_cache = &cache;
        merkle.BuildTree(d / "src", opts);
        cache.Save();
    }
    WriteFile(d / "src/a.txt", "omega"); // same size, new content, fresh mtime
    store::StatCache cache(d / "statcache");
    store::TreeOptions opts;
    opts.stat_cache = &cache;
    auto root = merkle.BuildTree(d / "src", opts);
    ASSERT_EQ(cache.hits(), 0u);
    ASSERT_EQ(merkle.Lookup(root, "a.txt").digest.hash, store::CasStore::ComputeDigest("omega").hash);
}

TEST(StatCacheDropsEntriesOfVanishedFiles) {
    TempDir d;
    WriteFile(d / "src/a.txt", "a");
    WriteFile(d / "src/b.txt", "b");
    Age(d / "src/a.txt");
    Age(d / "src/b.txt");
    auto cas = std::make_shared<store::CasStore>(d / "cas");
    store::MerkleBuilder merkle(cas);
    {
        store::StatCache cache(d / "statcache");
        store::TreeOptions opts;
        opts.stat_cache = &cache;
        merkle.BuildTree(d / "src", opts);
        cache.Save();
    }
    std::filesystem::remove(d / "src/b.txt");
    {
        store::StatCache cache(d / "statcache");
        store::TreeOptions opts;
        opts.stat_cache = &cache;
        merkle.BuildTree(d / "src", opts);
        cache.Save();
    }
    WriteFile(d / "src/b.txt", "b");
    Age(d / "src/b.txt");
    store::StatCache cache(d / "statcache");
    store::TreeOptions opts;
    opts.stat_cache = &cache;
    merkle.BuildTree(d / "src", opts);
    ASSERT_EQ(cache.hits(), 1u);   // a.txt
    ASSERT_EQ(cache.misses(), 1u); // b.txt was forgotten when it vanished
}

TEST(DenyRulesCoverSecretsAtAnyDepth) {
    const auto rules = store::DenyRules({});
    for (const char* p :
         {".ssh", "home/me/.ssh", ".gnupg", ".aws/credentials", ".netrc", ".git-credentials", "proj/.env",
          "proj/.env.local", ".envrc", ".kube/config", ".docker/config.json", ".bash_history",
          "home/me/.local/share/zeromesh/node.key", ".config/zeromesh/donor.toml",
          ".local/share/keyrings/login.keyring", ".config/gh/hosts.yml", ".password-store/x.gpg", ".git/HEAD"}) {
        if (!rules.Ignored(p, false)) throw std::runtime_error(std::string("not denied: ") + p);
    }
    ASSERT_FALSE(rules.Ignored("src/main.c", false));
    ASSERT_FALSE(rules.Ignored("environment.yml", false));
}

TEST(DenyRulesReadUserFile) {
    TempDir d;
    WriteFile(d / "deny", "*.custom_secret\nsecrets/\n");
    const auto rules = store::DenyRules(d / "deny");
    ASSERT_TRUE(rules.Ignored(".ssh", true));
    ASSERT_TRUE(rules.Ignored("pass.custom_secret", false));
    ASSERT_TRUE(rules.Ignored("secrets", true));
    ASSERT_FALSE(rules.Ignored("hello.txt", false));
}
