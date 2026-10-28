#pragma once

#include <string>

namespace zeromesh::exec {

// Hex SHA-256 identity of the program a host-env task will run: the resolved executable,
// its shebang interpreter, its ELF loader and the closure of its DT_NEEDED libraries.
// Two nodes agree on the digest only if they would execute the same toolchain bytes.
//
// `exe` is argv[0]; a bare name is looked up in the colon-separated `path_env`.
// Returns "" when `exe` cannot be resolved to a regular file, so callers treat the
// toolchain as unknown rather than failing the task.
std::string ToolchainDigest(const std::string& exe, const std::string& path_env);

} // namespace zeromesh::exec
