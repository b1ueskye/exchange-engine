#include "exchange/engine.hpp"
#include "test_checks.hpp"

#include <stdexcept>

int main() {
    using namespace exchange;
    TestChecks checks;
    Engine engine;
    const SymbolSpec a{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 10, 2, 1, 3};
    auto b = a;
    b.symbol = SymbolId{2};
    checks.require(engine.execute(RegisterSymbol{a}).outcome.status == Status::processed &&
                   engine.execute(RegisterSymbol{b}).outcome.status == Status::processed, "two markets");

    const auto order = [](SymbolId symbol) {
        return PlaceOrder{{symbol, OrderId{7}}, UserId{10}, Side::bid, Quantity{1},
                          LimitPrice{Price{100}, LimitPolicy::gtc, std::nullopt}, 0};
    };
    const auto first = engine.execute(order(a.symbol));
    const auto second = engine.execute(order(b.symbol));
    checks.require(first.seq == Seq{3} && second.seq == Seq{4} &&
                   first.outcome.status == Status::processed &&
                   second.outcome.status == Status::processed, "sequence and same id");

    const auto missing = engine.execute(order(SymbolId{3}));
    checks.require(missing.seq == Seq{5} && missing.outcome.status == Status::unknown_symbol,
                   "missing market has a sequence");

    const auto saved = engine.book_orders(b.symbol);
    const auto cut = engine.execute(CancelOrder{{a.symbol, OrderId{7}}, UserId{10}});
    checks.require(cut.outcome.status == Status::processed && cut.outcome.events.size() == 1 &&
                   engine.book_orders(a.symbol) == std::vector<RestingOrder>{} &&
                   engine.book_orders(b.symbol) == saved, "cancel in one market only");

    bool refused = false;
    if (saved && !saved->empty()) {
        auto bad = *saved;
        bad.push_back(bad.front());
        try {
            OrderBook::from_resting(b.symbol, bad);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
    }
    checks.require(refused && engine.book_orders(b.symbol) == saved, "bad staging leaves live book intact");
    return checks.exit_code();
}