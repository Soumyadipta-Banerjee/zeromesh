#include "tests/unit/test_framework.hpp"

#include "plugins/frontend-reapi/heuristics.hpp"

using zeromesh::frontend_reapi::IsCriticalPath;

TEST(ReapiHeuristicsPinsCompilerDriverLink) {
    ASSERT_TRUE(IsCriticalPath({"/usr/bin/c++", "-o", "app", "main.o", "math.o"}));
    ASSERT_TRUE(IsCriticalPath({"clang++", "main.obj", "-o", "app.exe"}));
    ASSERT_FALSE(IsCriticalPath({"/usr/bin/c++", "-o", "main.o", "-c", "main.cpp"}));
    ASSERT_FALSE(IsCriticalPath({"/usr/bin/c++", "-E", "main.cpp"}));
}

TEST(ReapiHeuristicsPinsLinkAndArchiveTools) {
    ASSERT_TRUE(IsCriticalPath({"/usr/bin/ld.lld", "main.o", "-o", "app"}));
    ASSERT_TRUE(IsCriticalPath({"llvm-ar", "rcs", "libapp.a", "main.o"}));
    ASSERT_TRUE(IsCriticalPath({"gcc-ranlib", "libapp.a"}));
    ASSERT_FALSE(IsCriticalPath({"/usr/bin/c++", "-c", "main.cpp"}));
}
