#include "plugins/frontend-reapi/heuristics.hpp"

#include <string_view>

namespace zeromesh::frontend_reapi {

bool IsCriticalPath(const std::vector<std::string>& command_args) {
    if (command_args.empty()) return false;

    std::string_view binary = command_args[0];
    auto slash = binary.find_last_of('/');
    if (slash != std::string_view::npos) {
        binary = binary.substr(slash + 1);
    }

    // Linkers and fat archives are serial critical path bottlenecks
    if (binary == "ld" || binary == "ld.lld" || binary == "lld" || binary == "gold" || binary == "ar" ||
        binary == "gcc-ar" || binary == "llvm-ar" || binary == "ranlib" || binary == "gcc-ranlib" ||
        binary == "llvm-ranlib") {
        return true;
    }

    // Direct compiler invocation performing final link step (no -c / -E / -S flag)
    if (binary == "gcc" || binary == "g++" || binary == "c++" || binary == "cc" || binary == "clang" ||
        binary == "clang++") {
        bool has_compile_only = false;
        bool has_object_input = false;
        bool has_output = false;
        for (const auto& arg : command_args) {
            if (arg == "-c" || arg == "-E" || arg == "-S") {
                has_compile_only = true;
            }
            if ((arg.size() > 2 && arg.substr(arg.size() - 2) == ".o") ||
                (arg.size() > 4 && arg.substr(arg.size() - 4) == ".obj"))
                has_object_input = true;
            if (arg == "-o") has_output = true;
        }
        if (!has_compile_only && has_object_input && has_output) {
            return true; // linking step
        }
    }

    return false;
}

} // namespace zeromesh::frontend_reapi
