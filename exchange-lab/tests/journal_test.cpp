#include "exchange/persistence.hpp"
#include "test_checks.hpp"

#include <sstream>
#include <string>

int main() {
    using namespace exchange;
    TestChecks checks;
    const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 1, 1, 0, 0};
    const UserId user{1};
    const OrderKey key{spec.symbol, OrderId{7}};
    const std::vector<Command> commands{
        RegisterSymbol{spec}, AddUser{user}, AdjustBalance{user, {spec.quote, -7}, 2},
        SuspendUser{user}, ResumeUser{user}, RegisterSymbols{{spec}}, AddUsers{{user}},
        PlaceOrder{key, user, Side::bid, Quantity{1}, LimitPrice{Price{80}, LimitPolicy::ioc, Price{90}}, 9},
        PlaceOrder{key, user, Side::ask, Quantity{2}, BudgetLimit{NotionalSteps{160}}, 10},
        PlaceOrder{key, user, static_cast<Side>(256), Quantity{2}, BudgetLimit{NotionalSteps{160}}, 10},
        CancelOrder{key, user}, ReduceOrder{key, user, Quantity{1}}, MoveOrder{key, user, Price{75}},
    };
    for (const auto& command : commands) {
        const auto bytes = encode_record(Seq{4}, command);
        const auto parsed = scan_log(bytes);
        checks.require(parsed.records.size() == 1 && parsed.valid_bytes == bytes.size() &&
                       encode_record(parsed.records.front().seq, parsed.records.front().command) == bytes,
                       "record roundtrip");
    }

    Bytes log;
    JournaledEngine live{[&](auto bytes) { log.insert(log.end(), bytes.begin(), bytes.end()); }};
    const auto add = live.execute(AddUser{user});
    live.execute(QueryTotals{});
    const auto money = live.execute(AdjustBalance{user, {spec.quote, 500}, 1});
    const auto repeated = live.execute(AdjustBalance{user, {spec.quote, 500}, 1});
    const auto parsed = scan_log(log);
    Engine rebuilt;
    const auto replay = replay_log(rebuilt, parsed);
    checks.require(add.seq == Seq{1} && money.seq == Seq{3} && repeated.seq == Seq{4} &&
                   replay.size() == 3 && replay.at(1).seq == Seq{3} &&
                   replay.back().outcome.status == Status::duplicate_funding &&
                   rebuilt.next_sequence() == Seq{5}, "replay gap and business refusal");
    checks.require(rebuilt.ledger().available(user, spec.quote) == Money{spec.quote, 500},
                   "replay preserves account");

    std::ostringstream output{std::ios::binary};
    write_binary(output, log);
    std::istringstream input{output.str(), std::ios::binary};
    checks.require(read_binary(input) == log, "binary streams");
    auto short_tail = log;
    short_tail.pop_back();
    const auto prefix = scan_log(short_tail);
    checks.require(prefix.truncated_tail && prefix.records.size() == 2 &&
                   prefix.valid_bytes < short_tail.size(), "truncated final frame");

    const auto reject = [](const Bytes& bytes) {
        try { scan_log(bytes); } catch (const std::runtime_error&) { return true; }
        return false;
    };
    auto corrupt = log;
    corrupt.back() ^= std::byte{1};
    checks.require(reject(corrupt), "checksum error");
    auto version = log;
    version.at(4) = std::byte{2};
    checks.require(reject(version), "unknown version");
    auto length = log;
    length.at(8) = std::byte{0x80};
    checks.require(reject(length), "oversized length");
    auto repeated_seq = log;
    const auto last = encode_record(Seq{4}, AddUser{UserId{9}});
    repeated_seq.insert(repeated_seq.end(), last.begin(), last.end());
    checks.require(reject(repeated_seq), "repeated sequence");
    auto reversed_seq = log;
    const auto earlier = encode_record(Seq{2}, AddUser{UserId{9}});
    reversed_seq.insert(reversed_seq.end(), earlier.begin(), earlier.end());
    checks.require(reject(reversed_seq), "reversed sequence");

    JournaledEngine failed{[](auto) { throw std::runtime_error{"disk full"}; }};
    bool first = false;
    bool blocked = false;
    try { failed.execute(AddUser{user}); } catch (const std::runtime_error&) { first = true; }
    try { failed.execute(QueryTotals{}); } catch (const std::runtime_error&) { blocked = true; }
    checks.require(first && blocked && failed.failed() &&
                   failed.state().last_sequence() == Seq{0} &&
                   !failed.state().ledger().available(user, spec.quote), "write failure blocks commit");
    return checks.exit_code();
}
