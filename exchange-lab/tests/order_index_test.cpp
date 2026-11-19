#include "order_book_access.hpp"
#include "test_checks.hpp"
#include "trading_fixtures.hpp"

#include <iostream>
#include <new>

int main() {
    using namespace exchange;
    using Access = OrderBookTestAccess;
    using trading_fixture::rest;

    TestChecks checks;
    OrderBook book{SymbolId{1}};
    const auto a = rest(7, Side::ask, 101, 3, 1, 300);
    const auto b = rest(8, Side::ask, 101, 2, 2, 200);
    const auto c = rest(9, Side::ask, 101, 4, 3, 250);
    const auto d = rest(10, Side::ask, 101, 1, 4, 400);

    checks.require(book.validate(), "empty book is valid");
    for (const auto& order : {a, b, c, d}) {
        checks.require(Access::insert(book, order) == Status::processed && book.validate(),
                       "indexed insertion");
    }

    const auto buckets = Access::index_bucket_count(book);
    Access::rehash_index(book, buckets + 1);
    checks.require(Access::index_bucket_count(book) > buckets &&
                   book.find_order(b.key.order) == b && book.validate(),
                   "index rehash keeps list handles valid");

    checks.require(!Access::remove(book, b.key.order, UserId{99}) && book.validate(),
                   "wrong owner cannot remove");
    checks.require(!Access::remove(book, OrderId{999}, UserId{99}) && book.validate(),
                   "unknown order cannot remove");

    const auto fail = [] { throw std::bad_alloc{}; };
    const auto expect_rollback = [&](const RestingOrder& candidate) {
        const auto original = Access::queue(book, Side::ask, Price{101});
        const auto original_total = Access::total(book, Side::ask, Price{101});
        bool thrown = false;
        try {
            Access::inject_failure(book, candidate, fail);
        } catch (const std::bad_alloc&) {
            thrown = true;
        }
        return thrown && book.validate() && !book.find_order(candidate.key.order) &&
               Access::queue(book, Side::ask, Price{101}) == original &&
               Access::total(book, Side::ask, Price{101}) == original_total;
    };
    checks.require(expect_rollback(rest(11, Side::ask, 101, 5, 5, 500)),
                   "failed index insertion rolls back existing level");
    checks.require(expect_rollback(rest(12, Side::ask, 102, 5, 6, 600)) &&
                   !Access::total(book, Side::ask, Price{102}),
                   "failed index insertion erases new empty level");

    const auto saved = book.find_order(b.key.order);
    checks.require(Access::remove(book, b.key.order, b.user) == b && book.validate() &&
                   !book.find_order(b.key.order) &&
                   Access::queue(book, Side::ask, Price{101}) ==
                       std::vector<RestingOrder>{a, c, d} &&
                   Access::total(book, Side::ask, Price{101}) == 8, "remove middle");
    checks.require(Access::remove(book, d.key.order, d.user) == d && book.validate(),
                   "remove tail");
    checks.require(Access::remove(book, a.key.order, a.user) == a && book.validate() &&
                   Access::queue(book, Side::ask, Price{101}) == std::vector<RestingOrder>{c},
                   "remove head");
    checks.require(Access::remove(book, c.key.order, c.user) == c && book.validate() &&
                   !book.best_ask() && !Access::total(book, Side::ask, Price{101}),
                   "remove last and erase level");
    checks.require(saved == b, "lookup owns its value after erase");
    checks.require(Access::insert(book, rest(8, Side::bid, 99, 1, 7, 700)) ==
                   Status::processed && book.validate(), "reuse inactive ID");

    if (checks.exit_code() == 0) {
        std::cout << "index, erase and rollback checked\n";
    }
    return checks.exit_code();
}