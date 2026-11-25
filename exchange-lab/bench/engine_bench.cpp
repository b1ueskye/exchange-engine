#include "exchange/engine.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace exchange;
using Clock = std::chrono::steady_clock;

const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 1, 1, 1, 2};
const UserId buyer{1};
const UserId seller{2};

struct Options {
    std::string_view mode{"mix"};
    BookKind kind{BookKind::list};
    std::size_t depth{100};
    std::size_t users{2};
    std::size_t cycles{120};
    std::size_t sweep{10};
    std::size_t trials{5};
    std::size_t other_depth{};
};

std::size_t number(std::string_view value) {
    std::size_t result{};
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result > 1'000'000) {
        throw std::invalid_argument{"invalid numeric argument"};
    }
    return result;
}

Options options(int argc, char** argv) {
    Options args;
    if (argc > 9) {
        throw std::invalid_argument{"too many arguments"};
    }
    if (argc > 1) args.mode = argv[1];
    if (argc > 2) {
        const std::string_view layout = argv[2];
        if (layout != "list" && layout != "pooled") {
            throw std::invalid_argument{"invalid layout"};
        }
        args.kind = layout == "list" ? BookKind::list : BookKind::pooled;
    }
    if (argc > 3) args.depth = number(argv[3]);
    if (argc > 4) args.users = number(argv[4]);
    if (argc > 5) args.cycles = number(argv[5]);
    if (argc > 6) args.sweep = number(argv[6]);
    if (argc > 7) args.trials = number(argv[7]);
    if (argc > 8) args.other_depth = number(argv[8]);
    if (args.users < 2 || !args.cycles || !args.sweep || !args.trials ||
        args.sweep > 1000 || args.trials > 100 ||
        (args.mode != "mix" && args.mode != "place" && args.mode != "cancel" &&
         args.mode != "reduce" && args.mode != "move" && args.mode != "sweep" &&
         args.mode != "reject" && args.mode != "settlement-failure")) {
        throw std::invalid_argument{"invalid workload"};
    }
    return args;
}

PlaceOrder order(std::uint64_t id, Side side, std::int64_t price,
                 std::int64_t qty, SymbolId symbol = spec.symbol) {
    return {{symbol, OrderId{id}}, side == Side::bid ? buyer : seller, side,
            Quantity{qty}, LimitPrice{Price{price}, LimitPolicy::gtc, {}}, 0};
}

void submit(Engine& engine, const Command& command) {
    if (engine.execute(command).outcome.status != Status::processed) {
        throw std::runtime_error{"benchmark setup rejected"};
    }
}

void seed(Engine& engine, const Options& args) {
    submit(engine, RegisterSymbol{spec});
    auto other = spec;
    other.symbol = SymbolId{2};
    if (args.other_depth) submit(engine, RegisterSymbol{other});
    for (std::size_t i = 1; i <= args.users; ++i) {
        const UserId user{i};
        submit(engine, AddUser{user});
        submit(engine, AdjustBalance{user, {spec.quote, 1'000'000'000}, 1});
    }
    submit(engine, AdjustBalance{seller, {spec.base, 1'000'000'000}, 2});
    const auto populate = [&engine](std::size_t depth, SymbolId symbol) {
        for (std::size_t i = 0; i < depth; ++i) {
            submit(engine, order(i + 10, Side::bid, 50 + i % 32, 1, symbol));
        }
    };
    populate(args.depth, spec.symbol);
    populate(args.other_depth, other.symbol);
    if (args.mode == "settlement-failure") {
        submit(engine, order(1, Side::bid, 1, 1));
        // Isolate the cheap bid from the background on the traded symbol.
        submit(engine, order(2, Side::bid, 100, 1));
    }
}

struct Step {
    Command command;
    bool measure{};
    Status status{Status::processed};
    std::size_t events{};
    std::size_t trades{};
};

std::vector<Step> workload(const Options& args) {
    std::vector<Step> steps;
    const auto mode = args.mode;
    if (mode == "settlement-failure" && (args.depth != 0 || args.sweep != 2)) {
        throw std::invalid_argument{"settlement-failure requires depth 0 and sweep 2"};
    }
    // A cycle restores working depth so later measurements see the same book size.
    const OrderKey key{spec.symbol, OrderId{3}};
    const auto add = [&](Command cmd, bool measure, std::size_t events = 0,
                         std::size_t trades = 0, Status status = Status::processed) {
        steps.push_back({std::move(cmd), measure, status, events, trades});
    };
    std::uint32_t rng = 2026;
    for (std::size_t i = 0; i < args.cycles; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const std::int64_t price = 85 + (rng >> 24) % 5;
        if (mode == "sweep") {
            for (std::size_t j = 0; j < args.sweep; ++j) {
                add(order(args.depth + 10 + j, Side::ask, 100 + j % 8, 1), false);
            }
            add(order(3, Side::bid, 107, args.sweep), true, args.sweep, args.sweep);
        } else if (mode == "settlement-failure") {
            add(PlaceOrder{key, seller, Side::ask, Quantity{2},
                           BudgetLimit{NotionalSteps{101}}, 0},
                true, 0, 0, Status::settlement_failed);
        } else if (mode == "reject") {
            add(CancelOrder{key, buyer}, true, 0, 0, Status::unknown_order);
        } else {
            add(order(3, Side::bid, price, 3), mode == "place" || mode == "mix");
            if (mode == "reduce" || mode == "mix") {
                add(ReduceOrder{key, buyer, Quantity{1}}, true, 1);
            }
            if (mode == "move" || mode == "mix") {
                // The default bid reserve is the initial limit. Move down a tick.
                add(MoveOrder{key, buyer, Price{price - 1}}, true);
            }
            add(CancelOrder{key, buyer}, mode == "cancel" || mode == "mix", 1);
        }
    }
    return steps;
}

struct Sample {
    std::size_t ops{};
    std::uint64_t nanos{};
    std::size_t trades{};
    std::vector<std::uint64_t> latency;
    std::uint64_t fingerprint{};
};

void check(const Step& step, const Result& result) {
    if (result.outcome.status != step.status ||
        result.outcome.events.size() != step.events) {
        throw std::runtime_error{"benchmark workload status or event count changed"};
    }
    const auto trades = std::count_if(result.outcome.events.begin(),
        result.outcome.events.end(), [](const Event& event) {
            return std::holds_alternative<Trade>(event);
        });
    if (static_cast<std::size_t>(trades) != step.trades) {
        throw std::runtime_error{"benchmark trade count changed"};
    }
}

Sample run(Engine& engine, const std::vector<Step>& steps, bool latency) {
    Sample sample;
    if (latency) sample.latency.reserve(steps.size());
    std::vector<Result> results;
    constexpr std::size_t batch = 256;
    results.reserve(batch);
    // Time consecutive measured commands together for throughput. Isolated
    // operations still incur two clock reads, so use mix for sustained throughput.
    for (std::size_t i = 0; i < steps.size();) {
        if (!steps[i].measure) {
            check(steps[i], engine.execute(steps[i].command));
            ++i;
            continue;
        }
        auto end = i + 1;
        while (!latency && end < steps.size() && end - i < batch &&
               steps[end].measure) ++end;
        const auto start = Clock::now();
        for (auto j = i; j < end; ++j) {
            results.push_back(engine.execute(steps[j].command));
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count();
        sample.nanos += elapsed;
        if (latency) sample.latency.push_back(elapsed);
        sample.ops += end - i;
        for (auto j = i; j < end; ++j) {
            check(steps[j], results[j - i]);
            sample.trades += steps[j].trades;
        }
        i = end;
        results.clear();
    }
    sample.fingerprint = std::get<StateFingerprint>(
        engine.execute(QueryFingerprint{}).report).value;
    return sample;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = options(argc, argv);
        const auto capacity = std::max(args.depth + args.sweep + 2, args.other_depth);
        const auto steps = workload(args);
        Engine throughput{args.kind, capacity};
        Engine latency{args.kind, capacity};
        seed(throughput, args);
        seed(latency, args);
        auto warmup_args = args;
        warmup_args.cycles = std::min(args.cycles, std::size_t{1000});
        const auto warmup = workload(warmup_args);
        run(throughput, warmup, false);
        run(latency, warmup, false);
        for (std::size_t trial = 1; trial <= args.trials; ++trial) {
            const auto fast = run(throughput, steps, false);
            auto timed = run(latency, steps, true);
            if (fast.fingerprint != timed.fingerprint) {
                throw std::runtime_error{"throughput and latency state differ"};
            }
            auto& values = timed.latency;
            std::sort(values.begin(), values.end());
            const auto quantile = [&values](std::size_t per_mille) {
                return values[(values.size() * per_mille + 999) / 1000 - 1];
            };
            std::cout << "mode=" << args.mode
                      << " book=" << (args.kind == BookKind::list ? "list" : "pooled")
                      << " depth=" << args.depth << " users=" << args.users
                      << " other_depth=" << args.other_depth << " sweep=" << args.sweep
                      << " capacity=" << capacity << " round=" << trial
                      << " ops=" << fast.ops << " trades=" << fast.trades
                      << " ops_per_s=" << 1e9 * fast.ops / fast.nanos
                      << " trades_per_s=" << 1e9 * fast.trades / fast.nanos
                      << " p50_ns=" << quantile(500) << " p95_ns=" << quantile(950)
                      << " p99_ns=" << quantile(990) << " p999_ns=" << quantile(999)
                      << " max_ns=" << values.back()
                      << " fingerprint=" << fast.fingerprint << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nusage: engine_bench "
                  << "[mix|place|cancel|reduce|move|sweep|reject|settlement-failure] "
                  << "[list|pooled] [depth] [users] [cycles] [sweep] [trials] [other_depth]\n";
        return 1;
    }
}
