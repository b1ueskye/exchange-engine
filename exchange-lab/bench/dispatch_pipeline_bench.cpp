#include "exchange/async_engine.hpp"
#include "exchange/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <string_view>

namespace {
struct Allocations {
    std::size_t count{};
    std::size_t bytes{};
};
thread_local Allocations* allocations = nullptr;
}

void* operator new(std::size_t size) {
    if (auto* pointer = std::malloc(size == 0 ? 1 : size)) {
        if (allocations) {
            ++allocations->count;
            allocations->bytes += size;
        }
        return pointer;
    }
    throw std::bad_alloc{};
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
using namespace exchange;
using Clock = std::chrono::steady_clock;

const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 1, 1, 1, 2};
const UserId buyer{1};
const UserId seller{2};

void verify(const Result& result, const CommandResult& expected) {
    if (result.outcome != expected) {
        throw std::runtime_error{"unexpected benchmark result"};
    }
}

void seed(Engine& engine, unsigned depth) {
    for (const Command& command : {
             Command{RegisterSymbol{spec}}, Command{AddUsers{{buyer, seller}}},
             Command{AdjustBalance{buyer, {spec.quote, 100'000'000}, 1}},
             Command{AdjustBalance{seller, {spec.base, 100'000}, 1}}}) {
        verify(engine.execute(command), {Status::processed, {}});
    }
    for (unsigned i = 0; i < depth; ++i) {
        verify(engine.execute(PlaceOrder{
            {spec.symbol, OrderId{10 + i}}, seller, Side::ask, Quantity{1},
            LimitPrice{Price{110}, LimitPolicy::gtc, {}}, 0}), {Status::processed, {}});
    }
}

std::int64_t percentile(std::vector<std::int64_t>& samples, unsigned percent) {
    if (samples.empty()) {
        throw std::runtime_error{"no accepted submissions"};
    }
    std::sort(samples.begin(), samples.end());
    return samples[(samples.size() * percent + 99) / 100 - 1];
}

void run(unsigned depth, unsigned rate, unsigned count, const char* journal_path) {
    auto engine = std::make_unique<Engine>();
    seed(*engine, depth);
    // A zero-fill IOC can be replayed after any number of queue drops.
    const OrderKey key{spec.symbol, OrderId{1}};
    const Command command = PlaceOrder{key, buyer, Side::bid, Quantity{1},
        LimitPrice{Price{100}, LimitPolicy::ioc, {}}, 0};
    const CommandResult expected{Status::processed,
        {RejectRemainder{key, buyer, Quantity{1}, RejectReason::ioc_remainder}}};
    for (int i = 0; i < 100; ++i) verify(engine->execute(command), expected);

    if (rate == 0) {
        Allocations measurement;
        for (unsigned i = 0; i < count; ++i) {
            allocations = &measurement;
            const auto result = engine->execute(command);
            allocations = nullptr;
            verify(result, expected);
        }
        std::cout << "depth=" << depth << " ops=" << count
                  << " allocations_per_op=" << double(measurement.count) / count
                  << " allocated_bytes_per_op=" << double(measurement.bytes) / count << '\n';
        return;
    }

    std::ofstream journal;
    std::shared_ptr<Engine> core;
    std::shared_ptr<JournaledEngine> durable;
    if (journal_path) {
        journal.open(journal_path, std::ios::binary | std::ios::app);
        if (!journal) throw std::runtime_error{"cannot open benchmark journal"};
        durable = std::make_shared<JournaledEngine>(
            [&journal](std::span<const std::byte> bytes) { write_binary(journal, bytes); },
            std::move(engine));
    } else {
        core = std::move(engine);
    }

    constexpr std::size_t capacity = 1024;
    AsyncEngine async{capacity, [&core, &durable](const Command& cmd) {
        return durable ? durable->execute(cmd) : core->execute(cmd);
    }};
    struct Ticket {
        Clock::time_point scheduled;
        std::future<Result> result;
    };
    std::vector<Ticket> tickets(count);
    std::vector<std::int64_t> samples;
    samples.reserve(count);
    std::atomic<unsigned> ready{};
    std::exception_ptr receive_error;
    Clock::time_point finished;
    std::jthread receiver([&] {
        try {
            for (unsigned i = 0; i < count; ++i) {
                for (auto seen = ready.load(std::memory_order_acquire); seen <= i;
                     seen = ready.load(std::memory_order_acquire)) {
                    ready.wait(seen, std::memory_order_acquire);
                }
                auto& ticket = tickets[i];
                if (ticket.result.valid()) {
                    const auto result = ticket.result.get();
                    finished = Clock::now();
                    samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        finished - ticket.scheduled).count());
                    verify(result, expected);
                }
            }
        } catch (...) {
            receive_error = std::current_exception();
        }
    });

    const auto start = Clock::now();
    for (unsigned i = 0; i < count; ++i) {
        auto& ticket = tickets[i];
        ticket.scheduled = start + std::chrono::nanoseconds{1'000'000'000LL * i / rate};
        while (Clock::now() < ticket.scheduled) {}
        auto submitted = async.submit(command);
        if (auto* future = std::get_if<std::future<Result>>(&submitted)) {
            ticket.result = std::move(*future);
        }
        ready.store(i + 1, std::memory_order_release);
        ready.notify_one();
    }
    receiver.join();
    async.close();
    if (receive_error) std::rethrow_exception(receive_error);
    const auto p99 = percentile(samples, 99);
    const auto duration = std::chrono::duration<double>(
        std::max(finished, tickets.back().scheduled) - start).count();
    std::cout << "journal=" << (journal_path != nullptr) << " depth=" << depth
              << " offered_per_s=" << rate << " offered=" << count
              << " completed=" << samples.size()
              << " full=" << count - samples.size()
              << " ops_per_s=" << samples.size() / duration
              << " p50_ns=" << percentile(samples, 50) << " p99_ns=" << p99 << '\n';
}

unsigned parse(std::string_view text) {
    unsigned value{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || value > 1'000'000) {
        throw std::invalid_argument{"invalid numeric argument"};
    }
    return value;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc > 5 || parse(argv[3]) == 0) {
            throw std::invalid_argument{"usage: dispatch_pipeline_bench depth rate count [journal-file]"};
        }
        run(parse(argv[1]), parse(argv[2]), parse(argv[3]), argc == 5 ? argv[4] : nullptr);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
