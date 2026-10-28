#pragma once

// Reads Make-style dependency files (`gcc -MD`, `clang -MD`, `gfortran -M`, and anything else that
// writes "targets: prerequisites"): the prerequisites are the files the command read.

#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::exec {

// Every prerequisite of every rule, in first-seen order, without duplicates. Handles line
// continuations, "\ " and "\#" escapes, "$$", and rules without prerequisites (gcc -MP).
std::vector<std::string> ParseDepfile(std::string_view text);

} // namespace zeromesh::exec
