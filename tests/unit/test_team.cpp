#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "net/team.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <string>

using zeromesh::net::TeamKey;

TEST(TeamPhraseRoundTrip) {
    for (int i = 0; i < 64; ++i) {
        TeamKey k = zeromesh::net::GenerateTeamKey();
        std::string phrase = zeromesh::net::FormatPhrase(k);
        ASSERT_EQ(phrase.size(), std::string("ZM-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXC").size());
        ASSERT_TRUE(phrase.starts_with("ZM-"));
        auto back = zeromesh::net::ParsePhrase(phrase);
        ASSERT_TRUE(back.has_value());
        ASSERT_TRUE(back->secret == k.secret);
    }
    // Known answers: big-endian value, two leading zero bits, check = value mod 37.
    TeamKey one{};
    one.secret[15] = 1;
    ASSERT_EQ(zeromesh::net::FormatPhrase(TeamKey{}), std::string("ZM-0000-0000-0000-0000-0000-0000-000"));
    ASSERT_EQ(zeromesh::net::FormatPhrase(one), std::string("ZM-0000-0000-0000-0000-0000-0000-011"));
    TeamKey ones{};
    ones.secret.fill(0xff);
    auto back = zeromesh::net::ParsePhrase(zeromesh::net::FormatPhrase(ones));
    ASSERT_TRUE(back.has_value() && back->secret == ones.secret);
}

TEST(TeamPhraseTypoRejected) {
    TeamKey k = zeromesh::net::GenerateTeamKey();
    std::string phrase = zeromesh::net::FormatPhrase(k);
    // Change each data symbol in turn: mod-37 catches every single-symbol substitution.
    for (size_t i = 3; i + 1 < phrase.size(); ++i) {
        if (phrase[i] == '-') continue;
        std::string typo = phrase;
        typo[i] = phrase[i] == '7' ? '6' : '7';
        ASSERT_FALSE(zeromesh::net::ParsePhrase(typo).has_value());
    }
    ASSERT_FALSE(zeromesh::net::ParsePhrase(phrase.substr(0, phrase.size() - 1)).has_value());
    ASSERT_FALSE(zeromesh::net::ParsePhrase(phrase + "0").has_value());
    ASSERT_FALSE(zeromesh::net::ParsePhrase("").has_value());
}

TEST(TeamPhraseCaseAndSpaces) {
    TeamKey k = zeromesh::net::GenerateTeamKey();
    std::string phrase = zeromesh::net::FormatPhrase(k);
    std::string loose;
    for (char c : phrase.substr(3)) {
        loose.push_back(c == '-' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    auto a = zeromesh::net::ParsePhrase("  " + loose + " ");
    ASSERT_TRUE(a.has_value() && a->secret == k.secret);
    auto b = zeromesh::net::ParsePhrase("zm " + loose);
    ASSERT_TRUE(b.has_value() && b->secret == k.secret);
    // Crockford reads O as 0 and I/L as 1.
    std::string confusable = phrase;
    std::replace(confusable.begin() + 3, confusable.end(), '0', 'O');
    std::replace(confusable.begin() + 3, confusable.end(), '1', 'l');
    auto c = zeromesh::net::ParsePhrase(confusable);
    ASSERT_TRUE(c.has_value() && c->secret == k.secret);
}

TEST(TeamIdStableAndPublic) {
    TeamKey a = zeromesh::net::GenerateTeamKey();
    TeamKey b = zeromesh::net::GenerateTeamKey();
    std::string id = zeromesh::net::TeamId(a);
    ASSERT_EQ(id.size(), static_cast<size_t>(32));
    ASSERT_TRUE(std::all_of(id.begin(), id.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)); }));
    ASSERT_EQ(zeromesh::net::TeamId(a), id);
    ASSERT_NE(zeromesh::net::TeamId(b), id);
    // Known answer: SHA-256("zm-team-id" || 16 zero bytes), first 16 bytes.
    ASSERT_EQ(zeromesh::net::TeamId(TeamKey{}), std::string("99fbd62a6300f7c7903dfd7a877a6fe3"));
}

TEST(TeamMacDiffersByLabel) {
    TeamKey k = zeromesh::net::GenerateTeamKey();
    auto m1 = zeromesh::net::Mac(k, "announce", "payload");
    ASSERT_TRUE(zeromesh::net::MacEqual(m1, zeromesh::net::Mac(k, "announce", "payload")));
    ASSERT_FALSE(zeromesh::net::MacEqual(m1, zeromesh::net::Mac(k, "admit", "payload")));
    ASSERT_FALSE(zeromesh::net::MacEqual(m1, zeromesh::net::Mac(k, "announce", "payloaD")));
    // The separator keeps label/data boundaries unambiguous.
    ASSERT_FALSE(zeromesh::net::MacEqual(zeromesh::net::Mac(k, "ab", "c"), zeromesh::net::Mac(k, "a", "bc")));
    ASSERT_FALSE(zeromesh::net::MacEqual(m1, zeromesh::net::Mac(zeromesh::net::GenerateTeamKey(), "announce", "payload")));
}

TEST(TeamFileMode0600) {
    zeromesh::testing::TempDir dir;
    auto state = dir / "state";
    ASSERT_FALSE(zeromesh::net::TeamFile::Load(state).has_value());
    TeamKey k = zeromesh::net::GenerateTeamKey();
    zeromesh::net::TeamFile::Save(state, k);
    struct stat st {};
    ASSERT_EQ(::stat((state / "team").c_str(), &st), 0);
    ASSERT_EQ(st.st_mode & 0777, static_cast<mode_t>(0600));
    auto loaded = zeromesh::net::TeamFile::Load(state);
    ASSERT_TRUE(loaded.has_value() && loaded->secret == k.secret);
    zeromesh::net::TeamFile::Remove(state);
    ASSERT_FALSE(zeromesh::net::TeamFile::Load(state).has_value());
}
