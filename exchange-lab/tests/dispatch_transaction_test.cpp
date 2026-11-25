#include "exchange/persistence.hpp"
#include "test_checks.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <random>

namespace {
thread_local int allocations_before_failure = -1;
}

void* operator new(std::size_t size) {
    if (allocations_before_failure == 0) {
        allocations_before_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) --allocations_before_failure;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using namespace exchange;

const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 1, 1, 1, 2};
const UserId buyer{1};
const UserId seller{2};

OrderKey key(std::uint64_t id) {
    return {spec.symbol, OrderId{id}};
}

PlaceOrder order(std::uint64_t id, UserId user, Side side, std::int64_t price,
                 std::int64_t lots, LimitPolicy policy = LimitPolicy::gtc) {
    return {key(id),
            user,
            side,
            Quantity{lots},
            LimitPrice{
                Price{price}, policy,
                side == Side::bid ? std::optional{Price{150}} : std::nullopt},
            static_cast<std::int64_t>(id)};
}

void fund(Engine& engine) {
    for (const Command& cmd :
         {Command{RegisterSymbol{spec}}, Command{AddUsers{{buyer, seller}}},
          Command{AdjustBalance{buyer, {spec.quote, 1'000'000}, 1}},
          Command{AdjustBalance{seller, {spec.base, 10'000}, 1}}}) {
        if (engine.execute(cmd).outcome.status != Status::processed) {
            throw std::logic_error{"dispatch fixture setup failed"};
        }
    }
}

std::uint64_t fingerprint(Engine& engine) {
    return std::get<StateFingerprint>(engine.execute(QueryFingerprint{}).report)
        .value;
}

void check_allocations(BookKind kind, bool move, TestChecks& checks) {
    Engine initial{kind, 32};
    fund(initial);
    for (const auto id : {4, 2, 3}) {
        const UserId maker{static_cast<std::uint64_t>(id + 10)};
        checks.require(
            initial.execute(AddUser{maker}).outcome.status ==
                    Status::processed &&
                initial.execute(AdjustBalance{maker, {spec.base, 5}, 1})
                        .outcome.status == Status::processed &&
                initial.execute(order(id, maker, Side::ask, 99 + id, 1))
                        .outcome.status == Status::processed,
            "seed maker with no existing quote balance");
    }
    const auto incoming = order(1, buyer, Side::bid, 110, 4);
    const Command cmd = move ? Command{MoveOrder{key(1), buyer, Price{110}}}
                             : Command{incoming};
    if (move) {
        checks.require(
            initial.execute(order(1, buyer, Side::bid, 95, 4)).outcome.status ==
                Status::processed,
            "seed move");
    }
    const auto starting = fingerprint(initial);
    const auto snapshot = SnapshotStore::encode(initial);
    auto completed = SnapshotStore::decode(snapshot);
    const auto expected = completed->execute(cmd);
    const auto final_state = fingerprint(*completed);
    checks.require(expected.outcome.status == Status::processed &&
                       expected.outcome.events.size() == 3,
                   "sweep and post remainder");

    for (int allocation = 0; allocation < 256; ++allocation) {
        auto engine = SnapshotStore::decode(snapshot);
        allocations_before_failure = allocation;
        try {
            const auto result = engine->execute(cmd);
            allocations_before_failure = -1;
            checks.require(allocation > 0 &&
                               result.outcome == expected.outcome &&
                               fingerprint(*engine) == final_state,
                           "completed transaction agrees with control");
            return;
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            checks.require(
                engine->last_sequence() == expected.seq &&
                    fingerprint(*engine) == starting,
                "allocation failure advances sequence but leaves state intact");
            const auto retry = engine->execute(cmd);
            const auto balances = engine->ledger().totals();
            checks.require(
                retry.outcome == expected.outcome &&
                    engine->book_orders(spec.symbol)->size() == 1 &&
                    engine->ledger().held(key(1)) == Money{spec.quote, 152} &&
                    std::all_of(
                        balances.currencies.begin(), balances.currencies.end(),
                        [](const auto& row) { return row.conserved(); }),
                "retry commits after an injected failure");
        }
    }
    checks.require(false, "allocation sweep reached success");
}

void trace(BookKind kind, std::ostream* snapshots) {
    Engine engine{kind, 32};
    fund(engine);
    std::mt19937 rng{20261125};
    for (unsigned i = 0; i < 5000; ++i) {
        const auto id = 1 + rng() % 64;
        const auto user = rng() % 3 == 0 ? seller : buyer;
        const auto side = rng() % 2 == 0 ? Side::ask : Side::bid;
        const auto qty = 1 + rng() % 7;
        const auto price = 85 + rng() % 35;
        auto place = order(id, user, side, price, qty);
        Command command = place;
        switch (rng() % 10) {
            case 0:
                place.constraint = BudgetLimit{
                    NotionalSteps{static_cast<std::int64_t>(price * qty)}};
                command = place;
                break;
            case 1:
                std::get<LimitPrice>(place.constraint).policy =
                    LimitPolicy::ioc;
                command = place;
                break;
            case 2:
            case 3:
                command = CancelOrder{key(id), user};
                break;
            case 4:
                command = ReduceOrder{key(id), user,
                                      Quantity{static_cast<std::int64_t>(qty)}};
                break;
            case 5:
            case 6:
                command = MoveOrder{key(id), user,
                                    Price{static_cast<std::int64_t>(price)}};
                break;
            default:
                break;
        }
        const auto result = engine.execute(command);
        std::cout << describe_result(result);
        for (const auto& event : result.outcome.events) {
            if (const auto* trade = std::get_if<Trade>(&event)) {
                std::cout << ' ' << trade->maker_user.value() << ' '
                          << trade->taker_user.value() << ' '
                          << (trade->taker_side == Side::bid) << ' '
                          << trade->maker_filled << ' ' << trade->taker_filled;
            } else if (const auto* reduce = std::get_if<Reduce>(&event)) {
                std::cout << ' ' << reduce->user.value() << ' '
                          << (reduce->side == Side::bid) << ' '
                          << reduce->price.value() << ' ' << reduce->removed;
            }
        }
        const auto state = SnapshotStore::encode(engine);
        if (snapshots) {
            snapshots->write(reinterpret_cast<const char*>(state.data()),
                             state.size());
        }
        std::cout << ' '
                  << wire::checksum(std::span<const std::byte>{state}.first(
                         state.size() - 4))
                  << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    TestChecks checks;
    std::ofstream snapshots;
    if (argc == 3) {
        snapshots.exceptions(std::ios::failbit | std::ios::badbit);
        snapshots.open(argv[2], std::ios::binary | std::ios::app);
    }
    for (const auto kind : {BookKind::list, BookKind::pooled}) {
        if (argc > 1) {
            trace(kind, snapshots.is_open() ? &snapshots : nullptr);
        } else {
            check_allocations(kind, false, checks);
            check_allocations(kind, true, checks);
        }
    }
    return checks.exit_code();
}
