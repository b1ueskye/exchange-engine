#include "exchange/async_engine.hpp"
#include "test_checks.hpp"

#include <algorithm>
#include <array>
#include <latch>
#include <stdexcept>
#include <vector>

int main() {
    using namespace exchange;
    TestChecks checks;
    std::latch running{1};
    std::latch finish{1};
    int steps = 0;
    AsyncEngine held{1, [&](const Command&) -> Result {
        if (++steps == 1) {
            running.count_down();
            finish.wait();
            throw std::runtime_error{"injected work error"};
        }
        return {Seq{static_cast<std::uint64_t>(steps)}, {Status::processed, {}}};
    }};
    auto first = held.submit(QueryTotals{});
    running.wait();
    auto second = held.submit(QueryTotals{});
    const auto full = held.submit(QueryTotals{});
    const auto* error = std::get_if<SubmitError>(&full);
    checks.require(error && *error == SubmitError::full, "deterministic backpressure");
    finish.count_down();
    std::jthread closer{[&] { held.close(); }};
    held.close();
    closer.join();
    auto after = held.submit(QueryTotals{});
    const auto* closed = std::get_if<SubmitError>(&after);
    checks.require(closed && *closed == SubmitError::closed, "closed rejects submission");
    bool threw = false;
    try {
        std::get<std::future<Result>>(first).get();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    checks.require(threw && std::get<std::future<Result>>(second).get().seq == Seq{2},
                   "exception delivered and accepted tail drained");

    AsyncEngine engine{64};
    struct Seen { AddUser command; std::future<Result> result; };
    std::array<std::vector<Seen>, 2> records;
    std::latch go{2};
    const auto send = [&](std::size_t lane) {
        go.arrive_and_wait();
        for (std::uint64_t i = 1; i <= 20; ++i) {
            const AddUser cmd{UserId{lane * 20 + i}};
            auto reply = engine.submit(cmd);
            records[lane].push_back({cmd, std::move(std::get<std::future<Result>>(reply))});
        }
    };
    std::jthread left{send, 0};
    std::jthread right{send, 1};
    left.join();
    right.join();
    auto fp = engine.submit(QueryFingerprint{});
    engine.close();

    std::vector<std::pair<Seq, AddUser>> replay;
    for (auto& lane : records) {
        for (auto& seen : lane) {
            const auto result = seen.result.get();
            checks.require(result.outcome.status == Status::processed, "accepted user command");
            replay.emplace_back(result.seq, seen.command);
        }
    }
    std::sort(replay.begin(), replay.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    Engine sync;
    for (const auto& [seq, cmd] : replay) {
        checks.require(sync.execute(cmd).seq == seq, "replay sequence");
    }
    const auto actual = std::get<std::future<Result>>(fp).get();
    checks.require(replay.size() == 40 && actual.seq == Seq{41} &&
                   std::get<StateFingerprint>(actual.report).value ==
                   std::get<StateFingerprint>(sync.execute(QueryFingerprint{}).report).value,
                   "two producers replay to the same state");

    AsyncEngine racing{8};
    std::latch race{2};
    std::array<Submission, 6> replies;
    std::jthread producer{[&] {
        race.arrive_and_wait();
        for (std::size_t i = 0; i < replies.size(); ++i) {
            replies[i] = racing.submit(AddUser{UserId{i + 1}});
        }
    }};
    race.arrive_and_wait();
    racing.close();
    producer.join();
    for (auto& reply : replies) {
        if (auto* future = std::get_if<std::future<Result>>(&reply)) {
            checks.require(future->get().outcome.status == Status::processed, "race accepted work");
        } else {
            checks.require(std::get<SubmitError>(reply) == SubmitError::closed, "race closed work");
        }
    }
    return checks.exit_code();
}
