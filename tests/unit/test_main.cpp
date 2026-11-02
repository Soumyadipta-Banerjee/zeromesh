#include "tests/unit/test_framework.hpp"

#include "net/tls.hpp"

int main() {
    zeromesh::net::PrepareGrpcRuntime();
    return zeromesh::testing::TestRegistry::Instance().RunAll();
}
