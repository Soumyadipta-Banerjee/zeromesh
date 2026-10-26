#include "tests/unit/test_framework.hpp"

int main() {
    return zeromesh::testing::TestRegistry::Instance().RunAll();
}
