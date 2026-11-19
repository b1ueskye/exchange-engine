#include "exchange/order_draft.hpp"
#include "test_checks.hpp"

#include <iostream>
#include <type_traits>

int main() {
    using exchange::OrderDraft;
    using exchange::Side;
    static_assert(!std::is_convertible_v<int, Side>);

    TestChecks checks;
    auto original = OrderDraft::create(7, Side::bid, 102, 3);
    checks.require(static_cast<bool>(original), "valid draft");
    if (!original) {
        return checks.exit_code();
    }

    checks.require(original->id() == 7 && original->price() == 102 &&
                   original->side() == Side::bid && original->quantity() == 3,
                   "initial draft fields");

    auto copy = *original;
    auto& alias = *original;
    checks.require(copy.set_quantity(5), "edit copy");
    checks.require(original->quantity() == 3, "original independent of copy");
    checks.require(alias.set_quantity(4), "edit alias");
    checks.require(original->quantity() == 4 && copy.quantity() == 5, "alias sees original");
    checks.require(!alias.set_quantity(0) && alias.quantity() == 4, "failed edit keeps quantity");

    checks.require(!OrderDraft::create(0, Side::bid, 102, 1), "invalid id");
    checks.require(!OrderDraft::create(8, Side::ask, -1, 1), "invalid price");
    checks.require(!OrderDraft::create(8, Side::ask, 102, 0), "invalid quantity");

    if (checks.exit_code() == 0) {
        std::cout << "All draft cases passed\n";
    }
    return checks.exit_code();
}