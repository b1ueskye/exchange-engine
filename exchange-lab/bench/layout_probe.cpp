#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <list>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t calls{};
    std::size_t bytes{};

private:
    void* do_allocate(std::size_t size, std::size_t align) override {
        auto* ptr = std::pmr::new_delete_resource()->allocate(size, align);
        ++calls;
        bytes += size;
        return ptr;
    }
    void do_deallocate(void* ptr, std::size_t size,
                       std::size_t align) override {
        std::pmr::new_delete_resource()->deallocate(ptr, size, align);
    }
    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct Packet {
    static inline int copies{};
    static inline int moves{};
    std::vector<int> events;

    explicit Packet(std::size_t count) : events(count, 7) {}
    Packet(const Packet& other) : events(other.events) {
        ++copies;
    }
    Packet(Packet&& other) noexcept : events(std::move(other.events)) {
        ++moves;
    }
};

Packet make_packet() {
    return Packet{64};
}

struct Row {
    std::uint64_t id;
    std::int64_t price;
    std::int64_t quantity;
    std::int64_t timestamp;
};

struct Columns {
    std::pmr::vector<std::uint64_t> ids;
    std::pmr::vector<std::int64_t> prices;
    std::pmr::vector<std::int64_t> quantities;
    std::pmr::vector<std::int64_t> timestamps;

    explicit Columns(std::pmr::memory_resource* resource)
        : ids(resource),
          prices(resource),
          quantities(resource),
          timestamps(resource) {}

    void reserve(std::size_t count) {
        ids.reserve(count);
        prices.reserve(count);
        quantities.reserve(count);
        timestamps.reserve(count);
    }

    void add(Row row) {
        ids.push_back(row.id);
        prices.push_back(row.price);
        quantities.push_back(row.quantity);
        timestamps.push_back(row.timestamp);
    }
};

template <class Rows>
std::int64_t sum_rows(const Rows& rows) {
    std::int64_t sum = 0;
    for (const auto& row : rows) {
        sum += row.quantity;
    }
    return sum;
}

template <class Scan, class Update>
void measure(const char* name, const CountingResource& resource, Scan scan,
             Update update, std::int64_t expected) {
    if (scan() != expected) {
        throw std::logic_error{"warmup checksum differs"};
    }
    double sum = 0;
    double squares = 0;
    constexpr int repeats = 12;
    for (int i = 0; i < repeats; ++i) {
        update(2 + i);
        const auto start = Clock::now();
        const auto observed = scan();
        const auto stop = Clock::now();
        if (observed != expected + 1 + i) {
            throw std::logic_error{"scan checksum differs"};
        }
        const double ns =
            std::chrono::duration<double, std::nano>(stop - start).count();
        sum += ns;
        squares += ns * ns;
    }
    const auto mean = sum / repeats;
    const auto variance = std::fmax(0.0, squares / repeats - mean * mean);
    std::cout << name << " allocations=" << resource.calls
              << " bytes=" << resource.bytes << " mean_ns=" << mean
              << " variance_ns2=" << variance << " base_checksum=" << expected
              << '\n';
}

}  // namespace

int main() {
    auto original = make_packet();
    auto copy = original;
    auto moved = std::move(original);
    if (copy.events != moved.events || Packet::copies != 1 ||
        Packet::moves != 1) {
        return 1;
    }
    original.events.assign(1, 9);
    std::cout << "copies=" << Packet::copies << " moves=" << Packet::moves
              << " sizeof_row=" << sizeof(Row)
              << " alignof_row=" << alignof(Row) << '\n';

    CountingResource contiguous_memory;
    CountingResource linked_memory;
    CountingResource column_memory;
    std::pmr::vector<Row> contiguous{&contiguous_memory};
    std::pmr::list<Row> linked{&linked_memory};
    Columns columns{&column_memory};
    constexpr std::size_t count = 65'536;
    contiguous.reserve(count);
    columns.reserve(count);
    std::int64_t expected = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Row row{static_cast<std::uint64_t>(i), 100,
                      static_cast<std::int64_t>(1 + i % 7), 0};
        contiguous.push_back(row);
        linked.push_back(row);
        columns.add(row);
        expected += row.quantity;
    }
    measure(
        "aos", contiguous_memory, [&] { return sum_rows(contiguous); },
        [&](std::int64_t value) { contiguous.front().quantity = value; },
        expected);
    measure(
        "list", linked_memory, [&] { return sum_rows(linked); },
        [&](std::int64_t value) { linked.front().quantity = value; }, expected);
    measure(
        "soa", column_memory,
        [&] {
            std::int64_t sum = 0;
            for (const auto quantity : columns.quantities) {
                sum += quantity;
            }
            return sum;
        },
        [&](std::int64_t value) { columns.quantities.front() = value; },
        expected);
}
