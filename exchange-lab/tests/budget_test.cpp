#include "matching_test_support.hpp"

#include <limits>

int main() {
    using namespace exchange;
    using namespace trading_fixture;

    TestChecks checks;
    const auto fixtures = cases();
    for (std::size_t i = 3; i < fixtures.size(); ++i) {
        matching_test::check_place(fixtures.at(i), checks);
    }

    const auto max = std::numeric_limits<std::int64_t>::max();
    const auto check_reject = [&](const std::vector<RestingOrder>& before, PlaceOrder order,
                                  RejectReason reason, const char* label) {
        OrderBook book{SymbolId{1}};
        matching_test::seed(book, before, checks);
        const CommandResult expected{Status::processed, {
            RejectRemainder{order.key, order.user, order.quantity, reason}}};
        checks.require(book.place(order, Seq{3}) == expected &&
                       book.resting_orders() == before && book.validate(), label);
    };
    check_reject({rest(1, Side::ask, 101, 1, 1, 0)},
                 budget(Side::bid, 500, 2), RejectReason::insufficient_liquidity, "no full fill");
    check_reject({rest(1, Side::ask, max, 2, 1, 0)},
                 budget(Side::bid, max, 2), RejectReason::arithmetic_limit, "product overflow");
    check_reject({rest(1, Side::ask, max - 1, 1, 1, 0), rest(2, Side::ask, max, 1, 2, 0)},
                 budget(Side::bid, max, 2), RejectReason::arithmetic_limit, "sum overflow");

    OrderBook edge{SymbolId{1}};
    const auto maker = rest(1, Side::ask, max, 1, 1, 0);
    matching_test::seed(edge, {maker}, checks);
    const auto order = budget(Side::bid, max, 1);
    checks.require(edge.place(order, Seq{2}) ==
                   CommandResult{Status::processed, {fill(maker, order, 1, true, true)}} &&
                   edge.resting_orders().empty() && edge.validate(), "maximum legal budget");
    return checks.exit_code();
}