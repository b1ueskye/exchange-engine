#ifndef EXCHANGE_PERSISTENCE_HPP
#define EXCHANGE_PERSISTENCE_HPP

#include "exchange/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace exchange {

using Bytes = std::vector<std::byte>;

namespace wire {

inline constexpr std::size_t max_payload = 1'048'576;
inline constexpr std::uint32_t max_batch = 4096;

struct Writer {
    Bytes data;
    void u(std::uint64_t value, unsigned width = 8) {
        for (unsigned i = 0; i < width; ++i) {
            data.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
        }
    }
    void i(std::int64_t value) {
        u(static_cast<std::uint64_t>(value));
    }
};

struct Reader {
    std::span<const std::byte> data;
    std::size_t pos{};

    std::size_t left() const {
        return data.size() - pos;
    }
    std::span<const std::byte> take(std::size_t size) {
        if (size > left()) {
            throw std::runtime_error{"truncated field"};
        }
        const auto value = data.subspan(pos, size);
        pos += size;
        return value;
    }
    std::uint64_t u(unsigned width = 8) {
        std::uint64_t result = 0;
        unsigned shift = 0;
        for (const auto value : take(width)) {
            result |= std::to_integer<std::uint64_t>(value) << shift;
            shift += 8;
        }
        return result;
    }
    std::int64_t i() {
        return static_cast<std::int64_t>(u());
    }
    bool flag() {
        const auto value = u(1);
        if (value > 1) {
            throw std::runtime_error{"invalid flag"};
        }
        return value != 0;
    }
    void end() const {
        if (left() != 0) {
            throw std::runtime_error{"unexpected trailing bytes"};
        }
    }
};

std::uint32_t checksum(std::span<const std::byte> bytes);
void write_spec(Writer& out, const SymbolSpec& spec);
SymbolSpec read_spec(Reader& in);

}  // namespace wire

struct LogRecord {
    Seq seq;
    Command command;
};
struct LogScan {
    std::vector<LogRecord> records;
    std::size_t valid_bytes{};
    bool truncated_tail{};
};

bool is_mutating(const Command& command);
Bytes encode_record(Seq seq, const Command& command);
LogScan scan_log(std::span<const std::byte> bytes);
Bytes read_binary(std::istream& stream);
void write_binary(std::ostream& stream, std::span<const std::byte> bytes);
std::vector<Result> replay_log(Engine& engine, const LogScan& log);

class SnapshotStore {
public:
    static Bytes encode(const Engine& engine);
    static std::unique_ptr<Engine> decode(std::span<const std::byte> bytes);
    static void publish(const std::filesystem::path& destination,
                        const Engine& engine);
};

class JournaledEngine {
public:
    using Append = std::function<void(std::span<const std::byte>)>;

    explicit JournaledEngine(Append append, BookKind kind = BookKind::list,
                             std::size_t capacity = 4096);
    JournaledEngine(Append append, std::unique_ptr<Engine> recovered);
    Result execute(const Command& command);
    bool failed() const {
        return failed_;
    }
    const Engine& state() const {
        return *engine_;
    }

private:
    std::unique_ptr<Engine> engine_;
    Append append_;
    bool failed_{};
};

}  // namespace exchange

#endif
