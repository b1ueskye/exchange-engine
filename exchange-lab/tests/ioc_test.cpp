#include "matching_test_support.hpp"

int main() {
    using namespace exchange;
    using namespace trading_fixture;

    TestChecks checks;
    const auto fixtures = cases();
    matching_test::check_place(fixtures.at(2), checks);

    OrderBook empty{SymbolId{1}};
    const auto six = limit(Side::bid, 102, 6, LimitPolicy::ioc);
    checks.require(empty.place(six, Seq{1}) == CommandResult{Status::processed,
                       {RejectRemainder{six.key, six.user, Quantity{6}, RejectReason::ioc_remainder}}} &&
                   empty.resting_orders().empty() && empty.validate(), "empty IOC");

    OrderBook exact{SymbolId{1}};
    const auto& initial = fixtures.at(2).before;
    matching_test::seed(exact, initial, checks);
    const auto five = limit(Side::bid, 102, 5, LimitPolicy::ioc);
    checks.require(exact.place(five, Seq{4}) == CommandResult{Status::processed,
                       {fill(initial.at(0), five, 3, true, false),
                        fill(initial.at(1), five, 2, true, true)}} &&
                   exact.resting_orders() == std::vector<RestingOrder>{initial.at(2)} &&
                   exact.validate(), "full IOC without rejection");
    return checks.exit_code();
}