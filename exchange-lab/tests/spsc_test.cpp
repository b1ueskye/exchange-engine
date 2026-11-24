#include "exchange/spsc_ring.hpp"
#include "test_checks.hpp"

#include <limits>
#include <thread>

int main() {
    using exchange::SpscRing;
    TestChecks checks;
    SpscRing<1> one{std::numeric_limits<std::uint64_t>::max()};
    checks.require(!one.try_pop() && one.try_push(9) && !one.try_push(10) &&
                   one.try_pop() == 9 && !one.try_pop() &&
                   one.try_push(10) && one.try_pop() == 10, "capacity one and counter wrap");

    SpscRing<8> ring{std::numeric_limits<std::uint64_t>::max() - 4};
    constexpr std::uint64_t count = 50'000;
    bool ordered = true;
    std::jthread writer{[&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!ring.try_push(i)) {
                std::this_thread::yield();
            }
        }
    }};
    std::jthread reader{[&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            auto next = ring.try_pop();
            while (!next) {
                std::this_thread::yield();
                next = ring.try_pop();
            }
            ordered = ordered && *next == i;
        }
    }};
    writer.join();
    reader.join();
    checks.require(ordered && !ring.try_pop(), "no loss, duplicate or reorder");
    return checks.exit_code();
}
