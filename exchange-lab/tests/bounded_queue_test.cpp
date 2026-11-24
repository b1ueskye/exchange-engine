#include "exchange/bounded_queue.hpp"
#include "test_checks.hpp"

#include <array>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

int main() {
    using namespace exchange;
    TestChecks checks;
    BoundedQueue<std::unique_ptr<int>> one{1};
    auto first = std::make_unique<int>(7);
    auto second = std::make_unique<int>(8);
    checks.require(one.try_push(std::move(first)) == PushStatus::accepted && !first,
                   "accepted transfers ownership");
    checks.require(one.try_push(std::move(second)) == PushStatus::full && second,
                   "full does not consume input");
    one.close();
    one.close();
    checks.require(one.try_push(std::move(second)) == PushStatus::closed && second,
                   "closed wins over full");
    const auto value = one.pop();
    checks.require(value && **value == 7 && !one.pop(), "drain before end");

    BoundedQueue<int> queue{2};
    std::latch start{3};
    std::vector<int> received;
    std::jthread reader{[&] {
        start.arrive_and_wait();
        while (auto item = queue.pop()) {
            received.push_back(*item);
        }
    }};
    const auto produce = [&](int base) {
        start.arrive_and_wait();
        for (int i = 0; i < 100; ++i) {
            int value = base + i;
            while (queue.try_push(std::move(value)) == PushStatus::full) {
                std::this_thread::yield();
            }
        }
    };
    std::jthread left{produce, 0};
    std::jthread right{produce, 100};
    left.join();
    right.join();
    queue.close();
    reader.join();

    std::array<int, 2> next{0, 100};
    bool ordered = true;
    for (const auto item : received) {
        const auto source = item < 100 ? 0 : 1;
        ordered = ordered && item == next.at(source)++;
    }
    checks.require(ordered && received.size() == 200 && next == std::array{100, 200},
                   "two producers preserve each sender order");

    BoundedQueue<int> idle{1};
    std::latch entered{1};
    bool ended = false;
    std::jthread waiting{[&] {
        entered.count_down();
        ended = !idle.pop();
    }};
    entered.wait();
    idle.close();
    waiting.join();
    checks.require(ended, "close wakes or preempts empty wait");
    return checks.exit_code();
}
