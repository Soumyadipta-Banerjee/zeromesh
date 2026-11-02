#include "exec/depfile.hpp"

#include <unordered_set>

namespace zeromesh::exec {

std::vector<std::string> ParseDepfile(std::string_view text) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    bool in_targets = true; // until the ':' of the current rule
    std::string token;
    bool have_token = false;

    auto flush = [&] {
        if (!have_token) return;
        if (!in_targets && seen.insert(token).second) out.push_back(token);
        token.clear();
        have_token = false;
    };

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '\\' && (next == '\n' || (next == '\r' && i + 2 < text.size() && text[i + 2] == '\n'))) {
            flush(); // a continuation is whitespace
            i += next == '\n' ? 1 : 2;
            continue;
        }
        if (c == '\\' && (next == ' ' || next == '#' || next == '\\' || next == ':')) {
            token.push_back(next);
            have_token = true;
            ++i;
            continue;
        }
        if (c == '$' && next == '$') {
            token.push_back('$');
            have_token = true;
            ++i;
            continue;
        }
        if (c == '\n' || c == '\r') {
            flush();
            in_targets = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            flush();
            continue;
        }
        // "target:" ends the targets when ':' is followed by whitespace or the end of the line
        // (a drive-letter "C:\x" never occurs in these files on Linux).
        if (c == ':' && in_targets && (next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '\0')) {
            token.clear(); // the token before ':' is a target
            have_token = false;
            in_targets = false;
            continue;
        }
        token.push_back(c);
        have_token = true;
    }
    flush();
    return out;
}

} // namespace zeromesh::exec
