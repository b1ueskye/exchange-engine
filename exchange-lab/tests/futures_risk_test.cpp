#include "exchange/futures_risk.hpp"
#include "trading_integration_fixtures.hpp"
#include "test_checks.hpp"
namespace exchange {
struct MarginTestAccess {
    static bool refused_record_keeps_tombstone(const Engine& e) {
        const auto& a = e.ledger().accounts_.at(UserId{1});
        return a.positions.empty() && a.position_order.slots().at(2) == 1 &&
               a.balances.contains(CurrencyId{20}) &&
               a.balances.at(CurrencyId{20}) == 0;
    }
};
}  // namespace exchange
int main() {
    using namespace integration_fixture;
    TestChecks checks;
    const auto s = spec(true);
    MarginPosition p{3, 300, 7, 2, 7};
    const LastPrice last{120, 90};
    checks.require(FuturesRisk::margin(p, s) == 500 &&
                       FuturesRisk::estimate(p, s, nullptr) == 307 &&
                       FuturesRisk::estimate(p, s, &last) == -23,
                   "Java margin and missing-price fallback");
    for (const auto kind : {BookKind::list, BookKind::pooled}) {
        Engine e{kind, 64};
        const auto commands = history(true, 7);
        for (std::size_t i = 0; i < commands.size(); ++i) {
            auto r = e.execute(commands[i]);
            if (const auto* place = std::get_if<PlaceOrder>(&commands[i]);
                place && place->key.order == OrderId{6}) {
                checks.require(r.outcome.status == Status::insufficient_funds,
                               "NSF has no trade");
            }
        }
        const auto user = e.execute(QueryUser{UserId{1}});
        const auto& report = std::get<UserReport>(user.report);
        checks.require(
            report.positions.empty() &&
                e.ledger().available(UserId{1}, s.quote) ==
                    Money{s.quote, 10100} &&
                e.ledger().fees(s.quote).units == 15,
            "empty position releases profit and charges opening quantity only");
        const auto totals = e.execute(QueryTotals{});
        for (const auto& row :
             std::get<TotalsReport>(totals.report).currencies) {
            checks.require(row.conserved(), "futures currency conservation");
        }
    }
    Engine fee_boundary;
    fee_boundary.execute(RegisterSymbol{s});
    fee_boundary.execute(AddUser{UserId{1}});
    fee_boundary.execute(AdjustBalance{UserId{1}, {s.quote, 100}, 1});
    checks.require(
        fee_boundary.execute(limit(1, 1, Side::bid, 1, 100)).outcome.status ==
            Status::processed,
        "exact margin admitted without extra fee reserve");
    checks.require(fee_boundary
                           .execute(MoveOrder{
                               {s.symbol, OrderId{1}}, UserId{1}, Price{200}})
                           .outcome.status == Status::processed,
                   "futures move ignores spot reserve price");
    fee_boundary.execute(CancelOrder{{s.symbol, OrderId{1}}, UserId{1}});
    checks.require(
        std::get<UserReport>(fee_boundary.execute(QueryUser{UserId{1}}).report)
            .positions.empty(),
        "cancel releases pending and removes empty record");
    Engine budget;
    budget.execute(RegisterSymbol{s});
    budget.execute(AddUsers{{UserId{1}, UserId{2}}});
    for (auto u : {UserId{1}, UserId{2}})
        budget.execute(AdjustBalance{u, {s.quote, 1000}, 1});
    budget.execute(limit(1, 2, Side::ask, 2, 100));
    auto fok = limit(2, 1, Side::bid, 2, 100);
    fok.constraint = BudgetLimit{NotionalSteps{200}};
    checks.require(budget.execute(fok).outcome.events.size() == 1 &&
                       budget.ledger().fees(s.quote).units == 6,
                   "futures FOK budget settles positions");
    fok.key.order = OrderId{3};
    checks.require(
        budget.execute(fok).outcome.events.size() == 1 &&
            std::get<UserReport>(budget.execute(QueryUser{UserId{1}}).report)
                    .positions.front()
                    .position.pending_buy == 0,
        "FOK rejection releases full pending");
    for (const bool cache : {false, true}) {
        Engine e{BookKind::list, 64, cache};
        auto f = s;
        f.maker_fee_per_lot = f.taker_fee_per_lot = 0;
        e.execute(RegisterSymbol{f});
        e.execute(AddUsers{{UserId{1}, UserId{2}, UserId{3}}});
        for (auto u : {UserId{1}, UserId{2}, UserId{3}})
            e.execute(
                AdjustBalance{u, {f.quote, u == UserId{2} ? 200 : 100}, 1});
        e.execute(limit(1, 2, Side::ask, 1, 100));
        e.execute(limit(2, 1, Side::bid, 1, 100));
        e.execute(limit(3, 3, Side::bid, 1, 90));
        const auto u = e.execute(QueryUser{UserId{1}});
        checks.require(
            std::get<UserReport>(u.report).positions.front().estimated_profit ==
                (cache ? -10 : 100),
            "estimate uses prior published BBO or exact Java fallback");
        checks.require(
            e.execute(limit(4, 1, Side::bid, 1, 80)).outcome.status ==
                (cache ? Status::insufficient_funds : Status::processed),
            "price cache changes admission");
    }
    Engine shared;
    auto f = s;
    f.margin_buy = f.margin_sell = 50;
    f.maker_fee_per_lot = f.taker_fee_per_lot = 0;
    auto spot = spec(false);
    spot.symbol = SymbolId{2};
    spot.maker_fee_per_lot = spot.taker_fee_per_lot = 0;
    shared.execute(RegisterSymbols{{f, spot}});
    shared.execute(AddUsers{{UserId{1}, UserId{2}}});
    for (auto u : {UserId{1}, UserId{2}})
        shared.execute(AdjustBalance{u, {s.quote, 100}, 1});
    shared.execute(limit(1, 2, Side::ask, 1, 100));
    shared.execute(limit(2, 1, Side::bid, 1, 100));
    shared.execute(limit(3, 2, Side::bid, 1, 200));
    auto cash_buy = limit(4, 1, Side::bid, 1, 120);
    cash_buy.key.symbol = spot.symbol;
    cash_buy.constraint = LimitPrice{Price{120}, LimitPolicy::gtc, Price{120}};
    checks.require(
        shared.execute(cash_buy).outcome.status == Status::processed &&
            shared.ledger().available(UserId{1}, s.quote)->units == -20,
        "spot hold can use same-currency futures surplus");
    shared.execute(CancelOrder{cash_buy.key, cash_buy.user});
    checks.require(shared.execute(AdjustBalance{UserId{1}, {s.quote, -100}, 2})
                           .outcome.status == Status::processed,
                   "Java withdrawal checks cash rather than required margin");
    Engine rejected;
    auto failed_spec = s;
    failed_spec.symbol = SymbolId{2};
    rejected.execute(RegisterSymbol{failed_spec});
    rejected.execute(AddUser{UserId{1}});
    auto failed = limit(1, 1, Side::bid, 1, 100);
    failed.key.symbol = failed_spec.symbol;
    checks.require(
        rejected.execute(failed).outcome.status == Status::insufficient_funds &&
            MarginTestAccess::refused_record_keeps_tombstone(rejected),
        "Java NSF cleanup preserves key-table history and zero balance slot");
    return checks.exit_code();
}
