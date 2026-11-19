#include "exchange/notional.hpp"
#include "test_checks.hpp"

#include <cstdint>
#include <iostream>
#include <limits>

int main() {
    using exchange::quote_notional;
    TestChecks checks;
    const auto max_amount = std::numeric_limits<std::int64_t>::max();

    const auto normal = quote_notional(3, 102, 10);
    checks.require(normal && *normal == 3060, "ordinary amount");

    const auto zero = quote_notional(0, max_amount, max_amount);
    checks.require(zero && *zero == 0, "zero quantity");

    const auto edge = quote_notional(max_amount, 1, 1);
    checks.require(edge && *edge == max_amount, "maximum amount");

    const auto rounded_bound = quote_notional(max_amount / 2, 2, 1);
    checks.require(rounded_bound && *rounded_bound == max_amount - 1,
                   "integer division bound");

    checks.require(!quote_notional(max_amount, 2, 1), "overflow multiplying by price");
    checks.require(!quote_notional(max_amount, 1, 2), "overflow multiplying by scale");
    checks.require(!quote_notional(-1, 1, 1), "negative quantity");
    checks.require(!quote_notional(0, 0, 1), "zero price even with zero quantity");
    checks.require(!quote_notional(1, 1, -1), "negative scale");
    checks.require(!quote_notional(1, 1, 0), "zero scale");

    if (checks.exit_code() == 0) {
        std::cout << "All notional cases passed\n";
    }
    return checks.exit_code();
}