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
    const OrderBook& read = book;
    checks.require(read.snapshot(3) == L2Snapshot{} && read.resting_orders().empty(),
                   "empty snapshot and order view");

    const auto fixtures = trading_fixture::cases();
    const auto& asks = fixtures.at(0).before;
    checks.require(Access::insert(book, asks.at(2)) == Status::processed, "insert worse ask first");
    checks.require(Access::insert(book, asks.at(0)) == Status::processed, "insert first best ask");
    checks.require(Access::insert(book, asks.at(1)) == Status::processed, "insert second best ask");
    checks.require(read.snapshot(1) ==
                   L2Snapshot{{{Price{101}, Quantity{5}, 2}}, {}}, "ask-only snapshot");

    const auto low_bid = rest(10, Side::bid, 99, 2, 4, 500);
    const auto high_bid = rest(11, Side::bid, 100, 1, 5, 600);
    checks.require(Access::insert(book, low_bid) == Status::processed, "insert low bid");
    checks.require(Access::insert(book, high_bid) == Status::processed, "insert high bid");

    const L2Snapshot top{{{Price{101}, Quantity{5}, 2}},
                         {{Price{100}, Quantity{1}, 1}}};
    const L2Snapshot full{{{Price{101}, Quantity{5}, 2}, {Price{103}, Quantity{4}, 1}},
                          {{Price{100}, Quantity{1}, 1}, {Price{99}, Quantity{2}, 1}}};
    const auto old_top = read.snapshot(1);
    const auto old_orders = read.resting_orders();
    checks.require(read.snapshot(0) == L2Snapshot{}, "depth zero");
    checks.require(old_top == top && read.snapshot(1) == old_top, "depth per side and repeat");
    checks.require(read.snapshot(std::numeric_limits<std::size_t>::max()) == full,
                   "depth larger than both sides");
    checks.require(old_orders == std::vector<RestingOrder>{
                       asks.at(0), asks.at(1), asks.at(2), high_bid, low_bid},
                   "canonical order view");

    const auto& a = asks.at(0);
    const auto& b = asks.at(1);
    checks.require(Access::remove(book, a.key.order, a.user) == a && book.validate(),
                   "remove first at best ask");
    checks.require(read.snapshot(1) ==
                   L2Snapshot{{{Price{101}, Quantity{2}, 1}}, top.bids},
                   "one order left at 101");
    checks.require(Access::remove(book, b.key.order, b.user) == b && book.validate(),
                   "remove last at best ask");
    checks.require(read.snapshot(1) ==
                   L2Snapshot{{{Price{103}, Quantity{4}, 1}}, top.bids},
                   "new best ask after level removal");
    checks.require(old_top == top && old_orders.size() == 5 &&
                   old_orders.at(0) == a && read.resting_orders().size() == 3,
                   "previous query values remain readable");

    const auto& c = asks.at(2);
    checks.require(Access::remove(book, c.key.order, c.user) == c &&
                   read.snapshot(1) == L2Snapshot{{}, top.bids} && book.validate(),
                   "bid-only snapshot after clearing asks");
    if (checks.exit_code() == 0) {
        std::cout << "L2 depth and owning order view checked\n";
    }
    return checks.exit_code();
}