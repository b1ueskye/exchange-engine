#include "matching_test_support.hpp"

#include <limits>

int main() {
    using namespace exchange;
    using namespace trading_fixture;

    TestChecks checks;
    const auto fixture = same_price_move();
    OrderBook same{SymbolId{1}};
    matching_test::seed(same, fixture.before, checks);
    checks.require(same.move(fixture.command, fixture.seq) == fixture.expected &&
                   same.resting_orders() == fixture.after && same.validate(), "same-price tail");

    const auto a = fixture.before.at(0);
    OrderBook book{SymbolId{1}};
    matching_test::seed(book, fixture.before, checks);
    const auto initial = book.resting_orders();
    checks.require(book.reduce({a.key, a.user, Quantity{0}}) ==
                   CommandResult{Status::invalid_input, {}} &&
                   book.cancel({a.key, UserId{99}}) == CommandResult{Status::unknown_order, {}} &&
                   book.resting_orders() == initial, "invalid size and wrong owner");
    checks.require(book.reduce({a.key, a.user, Quantity{1}}) ==
                   CommandResult{Status::processed, {
                       Reduce{a.key, a.user, a.side, a.price, Quantity{1}, false}}} &&
                   book.resting_orders().front().key == a.key && book.validate(),
                   "partial reduce keeps priority");
    checks.require(book.reduce({a.key, a.user, Quantity{99}}) ==
                   CommandResult{Status::processed, {
                       Reduce{a.key, a.user, a.side, a.price, Quantity{2}, true}}} &&
                   !book.find_order(a.key.order) && book.validate(), "excess reduce clips");

    OrderBook marketable{SymbolId{1}};
    const auto ask = rest(1, Side::ask, 103, 2, 1, 10);
    auto bid = rest(2, Side::bid, 99, 5, 2, 20);
    bid.reserve_bid_price = Price{105};
    matching_test::seed(marketable, {ask, bid}, checks);
    const auto before = marketable.resting_orders();
    checks.require(marketable.move({bid.key, bid.user, Price{106}}, Seq{3}) ==
                   CommandResult{Status::price_over_reserve, {}} &&
                   marketable.resting_orders() == before, "reserve bound before matching");
    checks.require(marketable.move({bid.key, bid.user, Price{104}}, Seq{3}) ==
                   CommandResult{Status::processed, {
                       Trade{ask.key, ask.user, bid.key, bid.user, Side::bid,
                             Price{103}, Quantity{2}, true, false}}} &&
                   marketable.best_bid() == Price{104} && !marketable.best_ask() &&
                   marketable.validate(), "move crosses and rests");
    checks.require(marketable.cancel({bid.key, bid.user}) ==
                   CommandResult{Status::processed, {
                       Reduce{bid.key, bid.user, Side::bid, Price{104}, Quantity{3}, true}}} &&
                   marketable.resting_orders().empty() && marketable.validate(),
                   "cancel remaining after partial fill");

    OrderBook fully_matched{SymbolId{1}};
    auto small_bid = bid;
    small_bid.remaining = Quantity{2};
    matching_test::seed(fully_matched, {ask, small_bid}, checks);
    checks.require(fully_matched.move({small_bid.key, small_bid.user, Price{103}}, Seq{3}) ==
                   CommandResult{Status::processed, {
                       Trade{ask.key, ask.user, small_bid.key, small_bid.user, Side::bid,
                             Price{103}, Quantity{2}, true, true}}} &&
                   fully_matched.resting_orders().empty() &&
                   !fully_matched.find_order(ask.key.order) &&
                   !fully_matched.find_order(small_bid.key.order) && fully_matched.validate(),
                   "fully matched move clears both ids");

    OrderBook overflow{SymbolId{1}};
    const auto max = std::numeric_limits<std::int64_t>::max();
    matching_test::seed(overflow, {rest(1, Side::ask, 102, max, 1, 10),
                                  rest(2, Side::ask, 103, 1, 2, 20)}, checks);
    const auto saved = overflow.resting_orders();
    checks.require(overflow.move({key(2), UserId{2}, Price{102}}, Seq{3}) ==
                   CommandResult{Status::invalid_input, {}} &&
                   overflow.resting_orders() == saved && overflow.validate(), "move overflow keeps old node");
    checks.require(overflow.move({key(1), UserId{1}, Price{102}}, Seq{3}) ==
                   CommandResult{Status::processed, {}} &&
                   overflow.validate(), "same-level max total can requeue");
    return checks.exit_code();
}