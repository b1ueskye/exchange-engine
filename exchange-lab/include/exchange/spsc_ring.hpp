#ifndef EXCHANGE_SPSC_RING_HPP
#define EXCHANGE_SPSC_RING_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace exchange {

template <std::size_t Capacity>
class SpscRing {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0);
    static_assert(Capacity < (std::uint64_t{1} << 63));

public:
    explicit SpscRing(std::uint64_t initial = 0)
        : read_(initial), write_(initial) {}

    bool try_push(std::uint64_t value) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (write - read == Capacity) {
            return false;
        }
        slots_[write & (Capacity - 1)] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    std::optional<std::uint64_t> try_pop() {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        if (read == write) {
            return std::nullopt;
        }
        const auto value = slots_[read & (Capacity - 1)];
        read_.store(read + 1, std::memory_order_release);
        return value;
    }

private:
    std::array<std::uint64_t, Capacity> slots_{};
    alignas(64) std::atomic<std::uint64_t> read_;
    alignas(64) std::atomic<std::uint64_t> write_;
};

}  // namespace exchange

#endif
