#include "matching_test_support.hpp"

#include <limits>

int main() {
    using namespace exchange;
    using namespace trading_fixture;

    TestChecks checks;
    const auto fixtures = cases();
    matching_test::check_place(fixtures.at(0), checks);
    matching_test::check_place(fixtures.at(1), checks);

    OrderBook book{SymbolId{1}};
    auto ask = limit(Side::ask, 103, 4, LimitPolicy::gtc);
    ask.key = key(9);
    ask.user = UserId{9};
    checks.require(book.place(ask, Seq{1}) == CommandResult{Status::processed, {}} &&
                   book.best_ask() == Price{103}, "passive GTC into empty book");

    const auto saved = book.resting_orders();
    auto duplicate = limit(Side::bid, 104, 4, LimitPolicy::gtc);
    duplicate.key = ask.key;
    checks.require(book.place(duplicate, Seq{2}) == CommandResult{Status::duplicate_order, {}} &&
                   book.resting_orders() == saved, "duplicate rejected before matching");

    const auto first = rest(7, Side::ask, 101, 3, 2, 300);
    matching_test::seed(book, {first}, checks);
    const auto sweep = limit(Side::bid, 103, 7, LimitPolicy::gtc);
    checks.require(book.place(sweep, Seq{3}) == CommandResult{Status::processed,
                       {fill(first, sweep, 3, true, false), fill(saved.at(0), sweep, 4, true, true)}} &&
                   book.resting_orders().empty() && book.validate(), "multi-level exact fill");

    OrderBook full_level{SymbolId{1}};
    const auto max = std::numeric_limits<std::int64_t>::max();
    matching_test::seed(full_level, {rest(5, Side::bid, 99, max, 1, 100)}, checks);
    const auto before = full_level.resting_orders();
    checks.require(full_level.place(limit(Side::bid, 99, 1, LimitPolicy::gtc), Seq{2}) ==
                   CommandResult{Status::invalid_input, {}} &&
                   full_level.resting_orders() == before && full_level.validate(),
                   "overflow during remainder preparation");
    return checks.exit_code();
}