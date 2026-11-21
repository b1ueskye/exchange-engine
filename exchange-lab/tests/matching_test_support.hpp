#ifndef EXCHANGE_MATCHING_TEST_SUPPORT_HPP
#define EXCHANGE_MATCHING_TEST_SUPPORT_HPP

#include "order_book_access.hpp"
#include "test_checks.hpp"
#include "trading_fixtures.hpp"

namespace matching_test {

inline void seed(exchange::OrderBook& book, const std::vector<exchange::RestingOrder>& orders,
                 TestChecks& checks) {
    for (const auto& order : orders) {
        checks.require(exchange::OrderBookTestAccess::insert(book, order) ==
                       exchange::Status::processed, "seed book");
    }
}

inline void check_place(const trading_fixture::PlaceFixture& fixture, TestChecks& checks) {
    exchange::OrderBook book{fixture.command.key.symbol};
    seed(book, fixture.before, checks);
    const auto result = book.place(fixture.command, fixture.seq);
    checks.require(result == fixture.expected && book.resting_orders() == fixture.after &&
                   book.validate(), fixture.name);
}

}

#endif