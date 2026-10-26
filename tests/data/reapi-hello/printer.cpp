#include "printer.hpp"

#include <iostream>

namespace reapi_hello {
void print_banner(const std::string& msg) {
    std::cout << "[REAPI-HELLO] " << msg << std::endl;
}
} // namespace reapi_hello
