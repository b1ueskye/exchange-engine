#include "exchange/engine.hpp"
#include "matching_test_support.hpp"

int main() {
    using namespace exchange;
    using namespace trading_fixture;
    TestChecks checks;

    auto book = make_book(BookKind::pooled, SymbolId{1}, 2);
    auto ask = limit(Side::ask, 100, 1, LimitPolicy::gtc);
    ask.key = key(1);
    auto other = ask;
    other.key = key(2);
    other.constraint = LimitPrice{Price{102}, LimitPolicy::gtc, {}};
    checks.require(book->place(ask, Seq{1}).status == Status::processed &&
                       book->place(other, Seq{2}).status == Status::processed,
                   "fill pool");
    const auto before = book->resting_orders();
    const auto buy = limit(Side::bid, 101, 2, LimitPolicy::gtc);
    checks.require(book->place(buy, Seq{3}) ==
                           CommandResult{Status::capacity_exhausted, {}} &&
                       book->resting_orders() == before && book->validate(),
                   "capacity refusal is atomic");
    checks.require(
        book->move({other.key, other.user, Price{103}}, Seq{4}).status ==
                Status::processed &&
            book->validate(),
        "move uses preparation slot");
    auto cloned = book->clone();
    checks.require(
        cloned->place(buy, Seq{5}).status == Status::capacity_exhausted &&
            cloned->resting_orders() == book->resting_orders(),
        "clone preserves pool capacity");
    auto ioc = buy;
    ioc.constraint = LimitPrice{Price{101}, LimitPolicy::ioc, {}};
    const auto filled = cloned->place(ioc, Seq{6});
    checks.require(filled.status == Status::processed &&
                       filled.events.size() == 2 &&
                       cloned->resting_orders().size() == 1 &&
                       cloned->validate() && book->resting_orders().size() == 2,
                   "IOC needs no slot and clone is independent");

    Engine engine{BookKind::pooled, 1};
    const SymbolSpec spec{
        SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 1, 1, 0, 0};
    const UserId buyer{1};
    auto bid = limit(Side::bid, 99, 1, LimitPolicy::gtc);
    bid.user = buyer;
    const auto status = [&](const Command& command) {
        return engine.execute(command).outcome.status;
    };
    checks.require(status(RegisterSymbol{spec}) == Status::processed &&
                       status(AddUser{buyer}) == Status::processed &&
                       status(AdjustBalance{buyer, {spec.quote, 500}, 1}) ==
                           Status::processed &&
                       status(bid) == Status::processed,
                   "fund and fill engine book");
    const auto original = engine.book_orders(spec.symbol);
    auto extra = bid;
    extra.key = key(3);
    checks.require(status(extra) == Status::capacity_exhausted &&
                       engine.book_orders(spec.symbol) == original &&
                       engine.ledger().available(buyer, spec.quote) ==
                           Money{spec.quote, 401} &&
                       engine.ledger().held(bid.key) == Money{spec.quote, 99} &&
                       !engine.ledger().held(extra.key),
                   "capacity refusal also discards staged hold");
    return checks.exit_code();
}
