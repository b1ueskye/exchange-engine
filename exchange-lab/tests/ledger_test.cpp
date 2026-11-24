#include "exchange/engine.hpp"
#include "test_checks.hpp"

int main() {
    using namespace exchange;
    TestChecks checks;
    Engine engine;
    const SymbolSpec spec{
        SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 10, 2, 1, 3};
    const UserId buyer{1};
    const UserId seller{2};
    const auto status = [&](Command cmd) {
        return engine.execute(cmd).outcome.status;
    };
    checks.require(status(RegisterSymbol{spec}) == Status::processed &&
                       status(AddUser{buyer}) == Status::processed &&
                       status(AddUser{seller}) == Status::processed,
                   "setup");
    checks.require(status(AdjustBalance{buyer, {spec.quote, 500}, 2}) ==
                           Status::processed &&
                       status(AdjustBalance{buyer, {spec.quote, 500}, 2}) ==
                           Status::duplicate_funding &&
                       status(AdjustBalance{buyer, {spec.quote, 500}, 1}) ==
                           Status::stale_funding,
                   "funding applied once");
    const PlaceOrder order{{spec.symbol, OrderId{7}},
                           buyer,
                           Side::bid,
                           Quantity{2},
                           LimitPrice{Price{100}, LimitPolicy::gtc, Price{105}},
                           0};
    checks.require(
        status(order) == Status::processed &&
            engine.ledger().available(buyer, spec.quote) ==
                Money{spec.quote, 74} &&
            engine.ledger().held(order.key) == Money{spec.quote, 426},
        "freeze bid");
    const auto before = engine.book_orders(spec.symbol);
    auto other = order;
    other.key.order = OrderId{8};
    checks.require(status(order) == Status::duplicate_order &&
                       status(other) == Status::insufficient_funds &&
                       status(AdjustBalance{buyer, {spec.quote, -75}, 3}) ==
                           Status::insufficient_funds &&
                       engine.book_orders(spec.symbol) == before,
                   "failed operations do not commit");
    checks.require(status(AdjustBalance{buyer, {spec.quote, -74}, 3}) ==
                           Status::processed &&
                       status(SuspendUser{buyer}) == Status::account_not_empty,
                   "frozen funds prevent suspension");
    checks.require(status(SuspendUser{seller}) == Status::processed,
                   "suspend empty user");
    const PlaceOrder ask{{spec.symbol, OrderId{9}},
                         seller,
                         Side::ask,
                         Quantity{1},
                         LimitPrice{Price{110}, LimitPolicy::gtc, std::nullopt},
                         0};
    checks.require(status(ask) == Status::user_suspended &&
                       status(ResumeUser{seller}) == Status::processed &&
                       status(AdjustBalance{seller, {spec.base, 10}, 1}) ==
                           Status::processed &&
                       status(ask) == Status::processed &&
                       engine.ledger().held(ask.key) == Money{spec.base, 10},
                   "resume then freeze ask");
    return checks.exit_code();
}