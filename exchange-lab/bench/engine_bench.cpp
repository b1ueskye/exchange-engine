#include "exchange/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace exchange;
using Clock = std::chrono::steady_clock;

constexpr std::size_t units = 120;
constexpr std::uint32_t seed = 2026;
const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 1, 1, 1, 2};
const UserId buyer{1};
const UserId seller{2};

OrderKey key(std::uint64_t id) { return {spec.symbol, OrderId{id}}; }

PlaceOrder order(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty) {
    const auto user = side == Side::bid ? buyer : seller;
    const auto reserve = side == Side::bid ? std::optional<Price>{Price{120}} : std::nullopt;
    return {key(id), user, side, Quantity{qty},
            LimitPrice{Price{price}, LimitPolicy::gtc, reserve}, 0};
}

void submit(Engine& engine, const Command& command) {
    if (engine.execute(command).outcome.status != Status::processed) {
        throw std::runtime_error{"benchmark setup rejected"};
    }
}

std::unique_ptr<Engine> make_engine() {
    auto engine = std::make_unique<Engine>();
    submit(*engine, RegisterSymbol{spec});
    submit(*engine, AddUsers{{buyer, seller}});
    submit(*engine, AdjustBalance{buyer, {spec.quote, 100'000'000}, 1});
    submit(*engine, AdjustBalance{seller, {spec.base, 100'000}, 1});
    return engine;
}

struct Prepared {
    std::vector<std::unique_ptr<Engine>> engines;
    std::vector<Command> commands;
    std::size_t expected_events{};
    std::size_t expected_trades{};
};

Prepared prepare(std::string_view mode) {
    Prepared run;
    run.engines.reserve(mode == "sweep" ? units : 1);
    run.commands.reserve(mode == "mix" ? 4 * units : units);
    if (mode != "sweep") {
        run.engines.push_back(make_engine());
    }
    std::mt19937 rng{seed};
    for (std::size_t i = 0; i < units; ++i) {
        const auto id = static_cast<std::uint64_t>(i) + 10;
        if (mode == "place") {
            run.commands.emplace_back(order(id, Side::bid, 90, 1));
        } else if (mode == "cancel") {
            submit(*run.engines.front(), order(id, Side::bid, 90, 1));
            run.commands.emplace_back(CancelOrder{key(id), buyer});
            ++run.expected_events;
        } else if (mode == "sweep") {
            auto engine = make_engine();
            submit(*engine, order(1, Side::ask, 100, 1));
            submit(*engine, order(2, Side::ask, 101, 1));
            run.engines.push_back(std::move(engine));
            run.commands.emplace_back(order(3, Side::bid, 101, 2));
            run.expected_events += 2;
            run.expected_trades += 2;
        } else {
            const auto price = static_cast<std::int64_t>(90 + rng() % 5);
            run.commands.emplace_back(order(id, Side::bid, price, 3));
            run.commands.emplace_back(ReduceOrder{key(id), buyer, Quantity{1}});
            run.commands.emplace_back(MoveOrder{key(id), buyer, Price{price + 1}});
            run.commands.emplace_back(CancelOrder{key(id), buyer});
            run.expected_events += 2;
        }
    }
    return run;
}

struct Counts {
    std::size_t processed{};
    std::size_t events{};
    std::size_t trades{};
};

void collect(Counts& counts, const Result& result) {
    counts.processed += result.outcome.status == Status::processed;
    counts.events += result.outcome.events.size();
    for (const auto& event : result.outcome.events) {
        counts.trades += std::holds_alternative<Trade>(event);
    }
}

void verify(const Prepared& prepared, const Counts& counts) {
    if (counts.processed != prepared.commands.size() ||
        counts.events != prepared.expected_events || counts.trades != prepared.expected_trades) {
        throw std::runtime_error{"benchmark workload changed"};
    }
}

Engine& engine_at(Prepared& prepared, std::size_t index) {
    return *prepared.engines[prepared.engines.size() == 1 ? 0 : index];
}

void round(std::string_view mode, int trial, bool print) {
    auto throughput = prepare(mode);
    auto latency = prepare(mode);
    std::vector<std::int64_t> samples;
    samples.reserve(latency.commands.size());

    Counts fast;
    const auto begin = Clock::now();
    for (std::size_t i = 0; i < throughput.commands.size(); ++i) {
        collect(fast, engine_at(throughput, i).execute(throughput.commands[i]));
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    verify(throughput, fast);

    Counts timed;
    for (std::size_t i = 0; i < latency.commands.size(); ++i) {
        auto& engine = engine_at(latency, i);
        const auto& command = latency.commands[i];
        const auto start = Clock::now();
        const auto result = engine.execute(command);
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        samples.push_back(elapsed_ns);
        collect(timed, result);
    }
    verify(latency, timed);
    if (print) {
        std::sort(samples.begin(), samples.end());
        const auto rank = [&samples](std::size_t percentile) {
            return samples[(samples.size() * percentile + 99) / 100 - 1];
        };
        std::cout << "mode=" << mode << " round=" << trial << " seed=" << seed
                  << " ops=" << fast.processed << " events=" << fast.events
                  << " trades=" << fast.trades << " seconds=" << elapsed
                  << " ops_per_s=" << static_cast<double>(fast.processed) / elapsed
                  << " p50_ns=" << rank(50) << " p99_ns=" << rank(99)
                  << " max_ns=" << samples.back() << '\n';
    }
}

}

int main(int argc, char** argv) {
    const std::string_view mode = argc == 2 ? argv[1] : "mix";
    if (argc > 2 || (mode != "place" && mode != "cancel" && mode != "sweep" && mode != "mix")) {
        std::cerr << "usage: engine_bench [place|cancel|sweep|mix]\n";
        return 2;
    }
    round(mode, 0, false);
    for (int i = 1; i <= 3; ++i) {
        round(mode, i, true);
    }
}
