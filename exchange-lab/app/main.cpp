#include "exchange/notional.hpp"
#include <iostream>

int main() {
    std::int64_t quantity = 100;
    std::int64_t price = 50;
    std::int64_t quote_scale = 1000;

    auto notional = exchange::quote_notional(quantity, price, quote_scale);
    if (notional) {
        std::cout << "Notional: " << *notional << std::endl;
    } else {
        std::cout << "Invalid input or overflow occurred." << std::endl;
    }

    return 0;
}