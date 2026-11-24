#include "exchange/persistence.hpp"
#include "test_checks.hpp"

int main() {
    using namespace exchange;
    TestChecks checks;
    Bytes log;
    JournaledEngine live{[&](auto frame) { log.insert(log.end(), frame.begin(), frame.end()); },
                         BookKind::pooled, 16};
    const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 10, 2, 1, 3};
    const UserId buyer{1};
    const UserId seller{2};
    live.execute(RegisterSymbol{spec});
    live.execute(AddUsers{{buyer, seller}});
    live.execute(AdjustBalance{buyer, {spec.quote, 1000}, 1});
    live.execute(AdjustBalance{seller, {spec.base, 50}, 1});
    PlaceOrder ask{{spec.symbol, OrderId{7}}, seller, Side::ask, Quantity{1},
                   LimitPrice{Price{100}, LimitPolicy::gtc, {}}, 99};
    live.execute(ask);
    ask.key.order = OrderId{8};
    ask.timestamp_ns = 98;
    live.execute(ask);
    const auto checkpoint = SnapshotStore::encode(live.state());
    live.execute(QueryTotals{});
    const PlaceOrder buy{{spec.symbol, OrderId{9}}, buyer, Side::bid, Quantity{2},
                         LimitPrice{Price{105}, LimitPolicy::ioc, {}}, 0};
    const auto expected = live.execute(buy);
    const auto journal = scan_log(log);

    auto restored = SnapshotStore::decode(checkpoint);
    Engine pure{BookKind::pooled, 16};
    const auto with_tail = replay_log(*restored, journal);
    const auto from_start = replay_log(pure, journal);
    checks.require(with_tail.size() == 1 && with_tail.front().seq == Seq{8} &&
                   with_tail.front().outcome == expected.outcome &&
                   from_start.back().outcome == expected.outcome,
                   "checkpoint and full replay preserve tail events");
    checks.require(SnapshotStore::encode(*restored) == SnapshotStore::encode(live.state()) &&
                   SnapshotStore::encode(pure) == SnapshotStore::encode(live.state()),
                   "all three paths have identical canonical bytes");
    checks.require(std::get<Trade>(expected.outcome.events.at(0)).maker.order == OrderId{7} &&
                   std::get<Trade>(expected.outcome.events.at(1)).maker.order == OrderId{8},
                   "FIFO survived out-of-order timestamps");

    const auto saved = SnapshotStore::encode(*restored);
    auto corrupt = checkpoint;
    corrupt.back() ^= std::byte{1};
    bool refused = false;
    try {
        auto replacement = SnapshotStore::decode(corrupt);
        restored.swap(replacement);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    checks.require(refused && SnapshotStore::encode(*restored) == saved, "bad snapshot leaves live engine");
    return checks.exit_code();
}
