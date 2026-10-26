#include "math.hpp"
#include "printer.hpp"
#include "string_util.hpp"

#include <iostream>

int main() {
    auto message = reapi_hello::greet("World");
    reapi_hello::print_banner(message);
    int sum = reapi_hello::add(19, 23);
    int prod = reapi_hello::multiply(6, 7);
    std::cout << "Sum: " << sum << ", Product: " << prod << std::endl;
    if (sum == 42 && prod == 42) {
        std::cout << "SUCCESS" << std::endl;
        return 0;
    }
    return 1;
}
