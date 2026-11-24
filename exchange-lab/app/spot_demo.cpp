#include "exchange/engine.hpp"

#include <iostream>

int main() {
    using namespace exchange;
    Engine engine;
    const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 10, 2, 1, 3};
    const UserId buyer{1};
    const UserId seller{2};
    const OrderKey bid{spec.symbol, OrderId{8}};
    const auto show = [&](const Command& command) {
        const auto result = engine.execute(command);
        std::cout << describe_result(result) << '\n';
        return result.outcome.status == Status::processed;
    };

    if (!show(RegisterSymbols{{spec}}) || !show(AddUsers{{buyer, seller}}) ||
        !show(AdjustBalance{buyer, {spec.quote, 1000}, 1}) ||
        !show(AdjustBalance{seller, {spec.base, 20}, 1}) ||
        !show(PlaceOrder{{spec.symbol, OrderId{7}}, seller, Side::ask, Quantity{2},
                         LimitPrice{Price{100}, LimitPolicy::gtc, {}}, 0}) ||
        !show(PlaceOrder{bid, buyer, Side::bid, Quantity{3},
                         LimitPrice{Price{105}, LimitPolicy::gtc, {}}, 0}) ||
        !show(CancelOrder{bid, buyer})) {
        return 1;
    }

    const auto report = engine.execute(QueryTotals{});
    std::cout << "report seq=" << report.seq.value() << '\n';
    for (const auto& row : std::get<TotalsReport>(report.report).currencies) {
        std::cout << "currency=" << row.currency.value() << " available=" << row.available
                  << " frozen=" << row.frozen << " fees=" << row.fees
                  << " net=" << row.net_deposits << '\n';
        if (!row.conserved()) {
            return 1;
        }
    }
    return 0;
}
