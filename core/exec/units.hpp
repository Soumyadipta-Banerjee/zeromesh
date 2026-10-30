#pragma once

// Partial commit (v2 blueprint, claim 3). A task commits one finished unit of its work by
// writing it atomically into the directory named by $ZM_UNITS_DIR (stage under a dot-name,
// then rename to the unit id). The sandbox runner notices the new file, stores it in the CAS
// and reports it; a retry or re-run of the same task then finds the units already in place.

#include <string_view>

namespace zeromesh::exec {

// Inside the sandbox this directory is /work/<kUnitsDirName>.
inline constexpr const char* kUnitsDirName = ".zm_units";

// Unit ids are file names: 1-128 characters of [A-Za-z0-9._-], not starting with '.'
// (dot-names are the staging area for atomic writes).
inline bool IsValidUnitId(std::string_view id) {
    if (id.empty() || id.size() > 128 || id.front() == '.') return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

} // namespace zeromesh::exec
