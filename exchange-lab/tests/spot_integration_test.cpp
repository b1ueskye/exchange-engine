#include "exchange/engine.hpp"
#include "test_checks.hpp"

#include <algorithm>
#include <stdexcept>

int check_spot(exchange::BookKind kind) {
    using namespace exchange;
    TestChecks checks;
    Engine engine{kind, 64};
    const SymbolSpec a{
        SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 10, 2, 1, 3};
    auto b = a;
    b.symbol = SymbolId{2};
    const UserId buyer{1};
    const UserId seller{2};
    const auto status = [&](Command cmd) {
        return engine.execute(cmd).outcome.status;
    };
    checks.require(status(RegisterSymbols{{a, b}}) == Status::processed &&
                       status(AddUsers{{buyer, seller}}) == Status::processed,
                   "batch initialization");
    auto c = a;
    c.symbol = SymbolId{3};
    checks.require(status(RegisterSymbols{{c, b}}) == Status::symbol_exists &&
                       !engine.book_orders(c.symbol),
                   "atomic symbol batch");
    checks.require(
        status(AddUsers{{UserId{3}, buyer}}) == Status::user_exists &&
            !engine.ledger().available(UserId{3}, a.quote),
        "atomic user batch");
    checks.require(
        status(AdjustBalance{buyer, {a.quote, 1000}, 1}) == Status::processed &&
            status(AdjustBalance{seller, {a.base, 50}, 1}) == Status::processed,
        "fund");

    const PlaceOrder ask{{a.symbol, OrderId{7}},
                         seller,
                         Side::ask,
                         Quantity{2},
                         LimitPrice{Price{100}, LimitPolicy::gtc, {}},
                         0};
    const PlaceOrder bid{{a.symbol, OrderId{8}},
                         buyer,
                         Side::bid,
                         Quantity{3},
                         LimitPrice{Price{105}, LimitPolicy::gtc, {}},
                         0};
    auto other = bid;
    other.key.symbol = b.symbol;
    other.quantity = Quantity{1};
    other.constraint = LimitPrice{Price{80}, LimitPolicy::gtc, {}};

    checks.require(status(ask) == Status::processed &&
                       status(bid) == Status::processed &&
                       status(other) == Status::processed,
                   "two markets with shared order id");
    const auto old = engine.execute(QueryUser{buyer});
    const auto* view = std::get_if<UserReport>(&old.report);
    checks.require(
        view && view->orders.size() == 2 &&
            view->frozen ==
                std::vector<FrozenOrder>{{bid.key, {a.quote, 213}},
                                         {other.key, {a.quote, 163}}},
        "report aligns holds and active orders");
    const auto first =
        std::get<StateFingerprint>(engine.execute(QueryFingerprint{}).report)
            .value;
    const auto second =
        std::get<StateFingerprint>(engine.execute(QueryFingerprint{}).report)
            .value;
    checks.require(first == second, "read does not change fingerprint");

    const auto cancelled = engine.execute(CancelOrder{bid.key, buyer});
    const auto publish = [](const Result&) {
        throw std::runtime_error{"subscriber error"};
    };
    bool caught = false;
    try {
        publish(cancelled);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    const auto current = engine.execute(QueryUser{buyer});
    const auto* now = std::get_if<UserReport>(&current.report);
    checks.require(
        caught && cancelled.outcome.status == Status::processed &&
            old.seq < cancelled.seq && cancelled.seq < current.seq && view &&
            view->frozen.size() == 2 && now &&
            now->frozen ==
                std::vector<FrozenOrder>{{other.key, {a.quote, 163}}} &&
            now->orders.size() == 1 && now->orders.front().key == other.key,
        "callback failure leaves committed cancel and old report");
    checks.require(
        std::get<StateFingerprint>(engine.execute(QueryFingerprint{}).report)
                .value != first,
        "write changes fingerprint");

    const auto totals = engine.execute(QueryTotals{});
    const auto& rows = std::get<TotalsReport>(totals.report).currencies;
    checks.require(rows.size() == 2 && std::all_of(rows.begin(), rows.end(),
                                                   [](const auto& row) {
                                                       return row.conserved();
                                                   }),
                   "conservation for both currencies");
    if (rows.size() == 2) {
        const auto& quote = rows.at(1);
        checks.require(quote.currency == a.quote && quote.available == 829 &&
                           quote.frozen == 163 && quote.fees == 8 &&
                           quote.net_deposits == 1000,
                       "quote conservation details");
    }
    return checks.exit_code();
}

int main() {
    return check_spot(exchange::BookKind::list) |
           check_spot(exchange::BookKind::pooled);
}
