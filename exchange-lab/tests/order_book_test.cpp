#include "order_book_access.hpp"
#include "test_checks.hpp"
#include "trading_fixtures.hpp"

#include <iostream>
#include <limits>

int main() {
    using namespace exchange;
    using Access = OrderBookTestAccess;
    using trading_fixture::rest;

    TestChecks checks;
    OrderBook book{SymbolId{1}};
    checks.require(!book.best_ask() && !book.best_bid(), "empty book");

    const auto cases = trading_fixture::cases();
    const auto& orders = cases.at(0).before;
    checks.require(Access::insert(book, orders.at(2)) == Status::processed, "insert 103 first");
    checks.require(Access::insert(book, orders.at(0)) == Status::processed, "insert first 101");
    checks.require(Access::insert(book, orders.at(1)) == Status::processed, "append second 101");
    checks.require(book.best_ask() == Price{101} && !book.best_bid(), "ask-only book");
    checks.require(Access::queue(book, Side::ask, Price{101}) ==
                   std::vector<RestingOrder>{orders.at(0), orders.at(1)}, "arrival order at 101");
    checks.require(Access::total(book, Side::ask, Price{101}) == 5, "101 total remaining");

    checks.require(Access::insert(book, rest(10, Side::bid, 99, 2, 4, 500)) ==
                   Status::processed, "insert bid 99");
    checks.require(Access::insert(book, rest(11, Side::bid, 100, 1, 5, 600)) ==
                   Status::processed, "insert bid 100");
    checks.require(book.best_bid() == Price{100} && book.best_ask() == Price{101}, "best prices");
    checks.require(Access::insert(book, rest(7, Side::bid, 99, 1, 6, 700)) ==
                   Status::duplicate_order, "duplicate id on the other side");
    checks.require(Access::insert(book, rest(12, Side::bid, 101, 1, 6, 700)) ==
                   Status::invalid_input, "crossed fixture is rejected");
    checks.require(Access::insert(book, rest(13, Side::ask, 101, 1, 1, 700)) ==
                   Status::invalid_input, "same-price sequence must advance");
    auto other = rest(14, Side::ask, 104, 1, 7, 700);
    other.key.symbol = SymbolId{2};
    checks.require(Access::insert(book, other) == Status::invalid_input, "wrong symbol");
    checks.require(!Access::total(book, Side::ask, Price{104}), "no unexpected level");

    OrderBook large{SymbolId{1}};
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    checks.require(Access::insert(large, rest(1, Side::ask, 101, max, 1, 100)) ==
                   Status::processed, "max level total");
    checks.require(Access::insert(large, rest(2, Side::ask, 101, 1, 2, 200)) ==
                   Status::invalid_input &&
                   Access::queue(large, Side::ask, Price{101}).size() == 1 &&
                   Access::total(large, Side::ask, Price{101}) == max, "overflow leaves the level unchanged");

    if (checks.exit_code() == 0) {
        std::cout << "best bid 100, best ask 101, FIFO 7 then 8\n";
    }
    return checks.exit_code();
}