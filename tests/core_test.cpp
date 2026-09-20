#include "pearl_core.h"
#include <exception>
#include <iostream>

int main() {
    try {
        const unsigned checks = pearl::self_test();
        std::cout << "PASS: " << checks << " reference checks; no mining or pool submission.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
