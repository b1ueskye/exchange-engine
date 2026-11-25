#include "exchange/async_engine.hpp"
#include "exchange/persistence.hpp"
#include "trading_integration_fixtures.hpp"
#include "test_checks.hpp"
#include <iostream>
#include <tuple>

namespace {
using namespace exchange;

bool same_report(const Report& a, const Report& b) {
    if (a.index() != b.index()) { return false; }
    return std::visit([&](const auto& left) {
        using T = std::decay_t<decltype(left)>;
        const auto& right = std::get<T>(b);
        if constexpr (std::is_same_v<T, std::monostate>) { return true; }
        else if constexpr (std::is_same_v<T, StateFingerprint>) { return left.value == right.value; }
        else if constexpr (std::is_same_v<T, UserReport>) {
            return left.user == right.user && left.suspended == right.suspended &&
                left.last_funding == right.last_funding && left.available == right.available &&
                left.frozen == right.frozen && left.orders == right.orders && left.positions == right.positions;
        } else if constexpr (std::is_same_v<T, TotalsReport>) {
            if (left.open_interest != right.open_interest) { return false; }
            if (left.currencies.size() != right.currencies.size()) { return false; }
            for (std::size_t i = 0; i < left.currencies.size(); ++i) {
                const auto& x = left.currencies[i]; const auto& y = right.currencies[i];
                if (std::tie(x.currency, x.available, x.frozen, x.fees, x.net_deposits,x.positions) !=
                    std::tie(y.currency, y.available, y.frozen, y.fees, y.net_deposits,y.positions)) { return false; }
            }
            return true;
        } else { return left == right; }
    }, a);
}

struct Spot {
    using State = Engine;
    using Journal = JournaledEngine;
    static const char* name() { return "spot"; }
    static auto fresh(BookKind kind) { return std::make_unique<State>(kind, 64); }
    static auto encode(const State& s) { return SnapshotStore::encode(s); }
    static auto decode(const Bytes& bytes) { return SnapshotStore::decode(bytes); }
    static auto replay(State& s, const LogScan& log) { return replay_log(s, log); }
    static auto history(std::uint32_t seed) { return integration_fixture::history(false,seed); }
};

struct Futures : Spot {
    static const char* name() { return "futures"; }
    static auto history(std::uint32_t seed) { return integration_fixture::history(true,seed); }
};

template<class Product>
void run(TestChecks& checks, BookKind kind, bool async, int mode, std::uint32_t seed) {
    const auto commands = Product::history(seed);
    auto reference = Product::fresh(kind);
    std::vector<Result> expected;
    for (const auto& c : commands) { expected.push_back(reference->execute(c)); }
    const auto expected_state = Product::encode(*reference);
    const auto cut = commands.size() / 2;
    const auto context = std::string{Product::name()} + " book=" +
        (kind == BookKind::list ? "list" : "pooled") + (async ? " async" : " sync") +
        " mode=" + std::to_string(mode) + " seed=" + std::to_string(seed);

    Bytes log, snapshot, final_state;
    auto state = std::shared_ptr<typename Product::State>{Product::fresh(kind)};
    std::shared_ptr<typename Product::Journal> journal;
    if (mode != 0) {
        journal = std::make_shared<typename Product::Journal>([&](std::span<const std::byte> b) {
            log.insert(log.end(), b.begin(), b.end());
        }, Product::fresh(kind));
    }
    std::size_t processed{};
    const AsyncEngine::Processor process = [&](const Command& c) {
        auto r = journal ? journal->execute(c) : state->execute(c);
        const auto& current = journal ? journal->state() : *state;
        ++processed;
        if (mode == 2 && processed == cut) { snapshot = Product::encode(current); }
        if (processed == commands.size()) { final_state = Product::encode(current); }
        return r;
    };
    std::unique_ptr<AsyncEngine> facade;
    if (async) { facade = std::make_unique<AsyncEngine>(8, process); }
    for (std::size_t i = 0; i < commands.size(); ++i) {
        Result result{Seq{0}, {Status::invalid_input, {}}};
        if (facade) {
            auto submission = facade->submit(commands[i]);
            if (!std::holds_alternative<std::future<Result>>(submission)) {
                checks.require(false, (context + " unexpected submit failure").c_str());
                facade->close(); return;
            }
            result = std::get<std::future<Result>>(submission).get();
        } else { result = process(commands[i]); }
        const auto& e = expected[i];
        checks.require(result.seq == e.seq && result.outcome == e.outcome && same_report(result.report, e.report),
                       (context + " first mismatch candidate seq=" + std::to_string(i + 1)).c_str());
    }
    if (facade) { facade->close(); }
    checks.require(final_state == expected_state, (context + " live state").c_str());
    if (mode == 0) { checks.require(log.empty() && snapshot.empty(), "memory-only has no persistence"); return; }
    auto recovered = mode == 2 ? Product::decode(snapshot) : Product::fresh(kind);
    const auto replayed = Product::replay(*recovered, scan_log(log));
    for (const auto& r : replayed) {
        const auto& e = expected.at(static_cast<std::size_t>(r.seq.value() - 1));
        checks.require(r.outcome == e.outcome && same_report(r.report, e.report), (context + " replay event").c_str());
    }
    checks.require(Product::encode(*recovered) == expected_state, (context + " recovered state").c_str());
}
}

int main() {
    TestChecks checks;
    std::size_t runs{};
    for (const auto seed : {7u, 31u, 2026u}) {
        for (const auto kind : {BookKind::list, BookKind::pooled}) {
            for (const bool async : {false, true}) {
                for (int mode = 0; mode < 3; ++mode) {
                    run<Spot>(checks, kind, async, mode, seed);
                    run<Futures>(checks, kind, async, mode, seed);
                    runs += 2;
                }
            }
        }
    }
    std::cout << runs << " scenario runs; 24 cells * 3 seeds; modes=memory,log,snapshot+log\n";
    return checks.exit_code();
}
