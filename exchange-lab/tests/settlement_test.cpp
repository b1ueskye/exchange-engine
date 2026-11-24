#include "exchange/engine.hpp"
#include "test_checks.hpp"

#include <limits>

using namespace exchange;

namespace {

const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 10, 2, 1, 3};
const UserId buyer{1};
const UserId seller{2};

Status status(Engine& engine, Command command) {
    return engine.execute(command).outcome.status;
}

void fund(Engine& engine, TestChecks& checks, const SymbolSpec& market = spec) {
    checks.require(status(engine, RegisterSymbol{market}) == Status::processed &&
                   status(engine, AddUser{buyer}) == Status::processed &&
                   status(engine, AddUser{seller}) == Status::processed &&
                   status(engine, AdjustBalance{buyer, {market.quote, 1000}, 1}) == Status::processed &&
                   status(engine, AdjustBalance{seller, {market.base, 50}, 1}) == Status::processed,
                   "fund buyer and seller");
}

PlaceOrder make(std::uint64_t id, UserId user, Side side, std::int64_t quantity,
                OrderConstraint constraint) {
    return {{spec.symbol, OrderId{id}}, user, side, Quantity{quantity}, constraint, 0};
}

}

int main() {
    TestChecks checks;
    const auto ask = make(7, seller, Side::ask, 2, LimitPrice{Price{100}, LimitPolicy::gtc, {}});
    const auto bid = make(8, buyer, Side::bid, 2, LimitPrice{Price{105}, LimitPolicy::gtc, {}});
    Engine buy_takes;
    fund(buy_takes, checks);
    checks.require(status(buy_takes, ask) == Status::processed &&
                   status(buy_takes, bid) == Status::processed &&
                   buy_takes.ledger().available(buyer, spec.base) == Money{spec.base, 20} &&
                   buy_takes.ledger().available(buyer, spec.quote) == Money{spec.quote, 594} &&
                   buy_takes.ledger().available(seller, spec.quote) == Money{spec.quote, 398} &&
                   buy_takes.ledger().fees(spec.quote) == Money{spec.quote, 8} &&
                   !buy_takes.ledger().held(bid.key), "426 equals 398 plus 20 plus 8");

    Engine sell_takes;
    fund(sell_takes, checks);
    auto passive = bid;
    passive.quantity = Quantity{3};
    passive.constraint = LimitPrice{Price{100}, LimitPolicy::gtc, Price{105}};
    checks.require(status(sell_takes, passive) == Status::processed &&
                   status(sell_takes, ask) == Status::processed &&
                   sell_takes.ledger().available(buyer, spec.quote) == Money{spec.quote, 385} &&
                   sell_takes.ledger().available(seller, spec.quote) == Money{spec.quote, 394} &&
                   sell_takes.ledger().held(bid.key) == Money{spec.quote, 213},
                   "maker fee and price improvement refund");
    checks.require(status(sell_takes, CancelOrder{bid.key, buyer}) == Status::processed &&
                   sell_takes.ledger().available(buyer, spec.quote) == Money{spec.quote, 598} &&
                   !sell_takes.ledger().held(bid.key), "cancel refunds remaining hold");

    Engine ioc;
    fund(ioc, checks);
    const auto three = make(8, buyer, Side::bid, 3, LimitPrice{Price{105}, LimitPolicy::ioc, {}});
    checks.require(status(ioc, ask) == Status::processed, "seed IOC");
    const auto partial = ioc.execute(three);
    checks.require(partial.outcome.events.size() == 2 &&
                   ioc.ledger().available(buyer, spec.quote) == Money{spec.quote, 594} &&
                   !ioc.ledger().held(three.key), "IOC refunds tail");

    Engine fok;
    fund(fok, checks);
    checks.require(status(fok, ask) == Status::processed, "seed FOK");
    auto budget = make(8, buyer, Side::bid, 2, BudgetLimit{NotionalSteps{199}});
    checks.require(status(fok, budget) == Status::processed &&
                   fok.ledger().available(buyer, spec.quote) == Money{spec.quote, 1000} &&
                   !fok.ledger().held(budget.key), "failed FOK refunds all");
    budget.constraint = BudgetLimit{NotionalSteps{210}};
    checks.require(status(fok, budget) == Status::processed &&
                   fok.ledger().available(buyer, spec.quote) == Money{spec.quote, 594},
                   "successful FOK refunds unused budget");

    Engine amend;
    fund(amend, checks);
    auto deeper = bid;
    deeper.quantity = Quantity{4};
    deeper.constraint = LimitPrice{Price{99}, LimitPolicy::gtc, Price{105}};
    checks.require(status(amend, ask) == Status::processed &&
                   status(amend, deeper) == Status::processed &&
                   status(amend, MoveOrder{bid.key, buyer, Price{100}}) == Status::processed &&
                   amend.ledger().held(bid.key) == Money{spec.quote, 426}, "partial move keeps reserve");
    checks.require(status(amend, ReduceOrder{bid.key, buyer, Quantity{1}}) == Status::processed &&
                   amend.ledger().held(bid.key) == Money{spec.quote, 213} &&
                   amend.ledger().available(buyer, spec.quote) == Money{spec.quote, 381},
                   "reduce refunds one lot");

    Engine same_user;
    fund(same_user, checks);
    auto own_ask = ask;
    own_ask.user = buyer;
    checks.require(status(same_user, AdjustBalance{buyer, {spec.base, 20}, 2}) == Status::processed &&
                   status(same_user, own_ask) == Status::processed &&
                   status(same_user, bid) == Status::processed &&
                   same_user.ledger().available(buyer, spec.base) == Money{spec.base, 20} &&
                   same_user.ledger().available(buyer, spec.quote) == Money{spec.quote, 992} &&
                   same_user.ledger().fees(spec.quote) == Money{spec.quote, 8}, "same account both roles");

    const auto max = std::numeric_limits<std::int64_t>::max();
    auto huge = bid;
    huge.constraint = LimitPrice{Price{max}, LimitPolicy::gtc, {}};
    const auto saved = amend.book_orders(spec.symbol);
    huge.key.order = OrderId{99};
    checks.require(status(amend, huge) == Status::invalid_input &&
                   amend.book_orders(spec.symbol) == saved &&
                   amend.ledger().available(buyer, spec.quote) == Money{spec.quote, 381},
                   "overflow before committing");

    Engine rejected;
    auto cheap = spec;
    cheap.quote_scale = 1;
    cheap.maker_fee_per_lot = 0;
    fund(rejected, checks, cheap);
    const auto low_bid = make(9, buyer, Side::bid, 1, LimitPrice{Price{1}, LimitPolicy::gtc, {}});
    const auto high_bid = make(11, buyer, Side::bid, 1, LimitPrice{Price{4}, LimitPolicy::gtc, {}});
    checks.require(status(rejected, low_bid) == Status::processed &&
                   status(rejected, high_bid) == Status::processed, "seed two bid levels");
    const auto original = rejected.book_orders(spec.symbol);
    const auto sell_budget = make(10, seller, Side::ask, 2, BudgetLimit{NotionalSteps{5}});
    const auto failed = rejected.execute(sell_budget);
    checks.require(failed.outcome == CommandResult{Status::settlement_failed, {}} &&
                   rejected.book_orders(spec.symbol) == original &&
                   rejected.ledger().available(seller, spec.base) == Money{spec.base, 50} &&
                   rejected.ledger().available(seller, spec.quote) == Money{spec.quote, 0} &&
                   rejected.ledger().available(buyer, spec.base) == Money{spec.base, 0} &&
                   rejected.ledger().available(buyer, spec.quote) == Money{spec.quote, 989} &&
                   rejected.ledger().fees(spec.quote) == Money{spec.quote, 0} &&
                   rejected.ledger().held(low_bid.key) == Money{spec.quote, 4} &&
                   rejected.ledger().held(high_bid.key) == Money{spec.quote, 7} &&
                   !rejected.ledger().held(sell_budget.key), "second trade refusal rolls back both");
    return checks.exit_code();
}
