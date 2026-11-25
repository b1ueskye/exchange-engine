#include "exchange/persistence.hpp"

#include <istream>
#include <limits>
#include <ostream>
#include <type_traits>
#include <utility>

namespace exchange {
namespace wire {

std::uint32_t checksum(std::span<const std::byte> bytes) {
    std::uint32_t hash = 2166136261u;
    for (const auto value : bytes) {
        hash = (hash ^ std::to_integer<std::uint32_t>(value)) * 16777619u;
    }
    return hash;
}

void write_spec(Writer& out, const SymbolSpec& spec) {
    out.u(spec.symbol.value(), 4);
    out.u(spec.base.value(), 4);
    out.u(spec.quote.value(), 4);
    out.i(spec.base_scale);
    out.i(spec.quote_scale);
    out.i(spec.maker_fee_per_lot);
    out.i(spec.taker_fee_per_lot);
    out.i(static_cast<std::int64_t>(spec.type)); out.i(spec.margin_buy); out.i(spec.margin_sell);
}

SymbolSpec read_spec(Reader& in, unsigned version) {
    SymbolSpec spec{SymbolId{static_cast<std::uint32_t>(in.u(4))},
        CurrencyId{static_cast<std::uint32_t>(in.u(4))},
        CurrencyId{static_cast<std::uint32_t>(in.u(4))}, in.i(), in.i(), in.i(), in.i()};
    if (version >= 2) {
        const auto raw_type = in.i();
        if (!std::in_range<std::underlying_type_t<SymbolType>>(raw_type)) {
            throw std::runtime_error{"symbol type outside underlying range"};
        }
        spec.type = static_cast<SymbolType>(raw_type); spec.margin_buy = in.i(); spec.margin_sell = in.i();
    }
    return spec;
}

}  // namespace wire

namespace {

using wire::Reader;
using wire::Writer;
constexpr std::uint32_t log_magic = 0x31474f4c;
constexpr std::uint8_t log_version = 2;
constexpr std::size_t log_header = 17;

void owner(Writer& out, OrderKey key, UserId user) {
    out.u(key.symbol.value(), 4);
    out.u(key.order.value());
    out.u(user.value());
}

OrderKey read_key(Reader& in) {
    return {SymbolId{static_cast<std::uint32_t>(in.u(4))}, OrderId{in.u()}};
}

std::uint32_t read_count(Reader& in) {
    const auto count = in.u(4);
    if (count > wire::max_batch) {
        throw std::runtime_error{"batch too large"};
    }
    return static_cast<std::uint32_t>(count);
}

void write_count(Writer& out, std::size_t count) {
    if (count > wire::max_batch) {
        throw std::length_error{"batch too large"};
    }
    out.u(count, 4);
}

template <class T>
inline constexpr bool is_query =
    std::is_same_v<T, QueryUser> || std::is_same_v<T, QueryTotals> ||
    std::is_same_v<T, QueryFingerprint>;

void encode_command(Writer& out, const Command& command) {
    std::visit(
        [&](const auto& cmd) {
            using T = std::decay_t<decltype(cmd)>;
            if constexpr (std::is_same_v<T, PlaceOrder>) {
                out.u(1, 1);
                owner(out, cmd.key, cmd.user);
                out.i(static_cast<std::int64_t>(cmd.side));
                out.i(cmd.quantity.value());
                out.i(cmd.timestamp_ns);
                if (const auto* limit =
                        std::get_if<LimitPrice>(&cmd.constraint)) {
                    out.u(1, 1);
                    out.i(limit->price.value());
                    out.i(static_cast<std::int64_t>(limit->policy));
                    out.u(limit->reserve_bid_price.has_value(), 1);
                    if (limit->reserve_bid_price) {
                        out.i(limit->reserve_bid_price->value());
                    }
                } else {
                    out.u(2, 1);
                    out.i(std::get<BudgetLimit>(cmd.constraint).total.value());
                }
            } else if constexpr (std::is_same_v<T, CancelOrder>) {
                out.u(2, 1);
                owner(out, cmd.key, cmd.user);
            } else if constexpr (std::is_same_v<T, ReduceOrder>) {
                out.u(3, 1);
                owner(out, cmd.key, cmd.user);
                out.i(cmd.quantity.value());
            } else if constexpr (std::is_same_v<T, MoveOrder>) {
                out.u(4, 1);
                owner(out, cmd.key, cmd.user);
                out.i(cmd.new_price.value());
            } else if constexpr (std::is_same_v<T, RegisterSymbol>) {
                out.u(5, 1);
                wire::write_spec(out, cmd.spec);
            } else if constexpr (std::is_same_v<T, AddUser>) {
                out.u(6, 1);
                out.u(cmd.user.value());
            } else if constexpr (std::is_same_v<T, AdjustBalance>) {
                out.u(7, 1);
                out.u(cmd.user.value());
                out.u(cmd.amount.currency.value(), 4);
                out.i(cmd.amount.units);
                out.u(cmd.funding_id);
            } else if constexpr (std::is_same_v<T, SuspendUser>) {
                out.u(8, 1);
                out.u(cmd.user.value());
            } else if constexpr (std::is_same_v<T, ResumeUser>) {
                out.u(9, 1);
                out.u(cmd.user.value());
            } else if constexpr (std::is_same_v<T, RegisterSymbols>) {
                out.u(10, 1);
                write_count(out, cmd.specs.size());
                for (const auto& spec : cmd.specs) {
                    wire::write_spec(out, spec);
                }
            } else if constexpr (std::is_same_v<T, AddUsers>) {
                out.u(11, 1);
                write_count(out, cmd.users.size());
                for (const auto user : cmd.users) {
                    out.u(user.value());
                }
            } else {
                static_assert(is_query<T>);
                throw std::invalid_argument{
                    "read command cannot enter the journal"};
            }
        },
        command);
}

template <class Enum>
Enum read_enum(Reader& in) {
    using Value = std::underlying_type_t<Enum>;
    const auto value = in.i();
    if (!std::in_range<Value>(value)) {
        throw std::runtime_error{"enum outside underlying type"};
    }
    return static_cast<Enum>(static_cast<Value>(value));
}

OrderConstraint read_constraint(Reader& in) {
    const auto type = in.u(1);
    if (type == 1) {
        const Price price{in.i()};
        const auto policy = read_enum<LimitPolicy>(in);
        std::optional<Price> reserve;
        if (in.flag()) {
            reserve = Price{in.i()};
        }
        return LimitPrice{price, policy, reserve};
    }
    if (type == 2) {
        return BudgetLimit{NotionalSteps{in.i()}};
    }
    throw std::runtime_error{"unknown constraint tag"};
}

Command decode_command(Reader& in, unsigned version) {
    switch (in.u(1)) {
        case 1: {
            const auto key = read_key(in);
            const UserId user{in.u()};
            const auto side = read_enum<Side>(in);
            const Quantity quantity{in.i()};
            const auto time = in.i();
            return PlaceOrder{key, user, side, quantity, read_constraint(in),
                              time};
        }
        case 2:
            return CancelOrder{read_key(in), UserId{in.u()}};
        case 3:
            return ReduceOrder{read_key(in), UserId{in.u()}, Quantity{in.i()}};
        case 4:
            return MoveOrder{read_key(in), UserId{in.u()}, Price{in.i()}};
        case 5:
            return RegisterSymbol{wire::read_spec(in, version)};
        case 6:
            return AddUser{UserId{in.u()}};
        case 7:
            return AdjustBalance{
                UserId{in.u()},
                {CurrencyId{static_cast<std::uint32_t>(in.u(4))}, in.i()},
                in.u()};
        case 8:
            return SuspendUser{UserId{in.u()}};
        case 9:
            return ResumeUser{UserId{in.u()}};
        case 10: {
            RegisterSymbols batch;
            const auto count = read_count(in);
            for (std::uint32_t i = 0; i < count; ++i) {
                batch.specs.push_back(wire::read_spec(in, version));
            }
            return batch;
        }
        case 11: {
            AddUsers batch;
            const auto count = read_count(in);
            for (std::uint32_t i = 0; i < count; ++i) {
                batch.users.push_back(UserId{in.u()});
            }
            return batch;
        }
        default:
            throw std::runtime_error{"unknown command tag"};
    }
}

}  // namespace

bool is_mutating(const Command& command) {
    return std::visit(
        [](const auto& cmd) { return !is_query<std::decay_t<decltype(cmd)>>; },
        command);
}

Bytes encode_record(Seq seq, const Command& command) {
    if (seq.value() == 0) {
        throw std::invalid_argument{"invalid log sequence"};
    }
    Writer payload;
    encode_command(payload, command);
    if (payload.data.size() > wire::max_payload) {
        throw std::length_error{"log payload too large"};
    }
    Writer out;
    out.u(log_magic, 4);
    out.u(log_version, 1);
    out.u(payload.data.size(), 4);
    out.u(seq.value());
    out.data.insert(out.data.end(), payload.data.begin(), payload.data.end());
    out.u(wire::checksum(out.data), 4);
    return std::move(out.data);
}

LogScan scan_log(std::span<const std::byte> bytes) {
    Reader in{bytes};
    LogScan result;
    Seq previous{0};
    while (in.left() != 0) {
        const auto start = in.pos;
        if (in.left() < log_header) {
            result.truncated_tail = true;
            break;
        }
        if (in.u(4) != log_magic) { throw std::runtime_error{"invalid log magic"}; }
        const auto version = static_cast<unsigned>(in.u(1));
        if (version != 1 && version != log_version) {
            throw std::runtime_error{"unsupported log header"};
        }
        const auto length = static_cast<std::size_t>(in.u(4));
        const Seq seq{in.u()};
        if (length == 0 || length > wire::max_payload || seq <= previous) {
            throw std::runtime_error{"invalid log length or sequence"};
        }
        if (in.left() < length + 4) {
            result.truncated_tail = true;
            break;
        }
        Reader payload{in.take(length)};
        const auto covered = bytes.subspan(start, in.pos - start);
        if (in.u(4) != wire::checksum(covered)) {
            throw std::runtime_error{"log checksum mismatch"};
        }
        auto command = decode_command(payload, version);
        payload.end();
        result.records.push_back({seq, std::move(command)});
        previous = seq;
        result.valid_bytes = in.pos;
    }
    return result;
}

Bytes read_binary(std::istream& stream) {
    Bytes bytes;
    char value = 0;
    while (stream.get(value)) {
        if (bytes.size() == 64u * 1024u * 1024u) {
            throw std::length_error{"binary input is too large"};
        }
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    if (stream.bad() || !stream.eof()) {
        throw std::runtime_error{"could not read binary stream"};
    }
    return bytes;
}

void write_binary(std::ostream& stream, std::span<const std::byte> bytes) {
    if (bytes.size() > static_cast<std::uintmax_t>(
                           std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error{"binary output is too large"};
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        throw std::runtime_error{"journal write or flush failed"};
    }
}

std::vector<Result> replay_log(Engine& engine, const LogScan& log) {
    std::vector<Result> results;
    const auto checkpoint = engine.last_sequence();
    for (const auto& record : log.records) {
        if (record.seq > checkpoint) {
            results.push_back(engine.execute_at(record.command, record.seq));
        }
    }
    return results;
}

JournaledEngine::JournaledEngine(Append append, BookKind kind,
                                 std::size_t capacity)
    : engine_(std::make_unique<Engine>(kind, capacity)),
      append_(std::move(append)) {}

Result JournaledEngine::execute(const Command& command) {
    if (failed_) {
        throw std::runtime_error{"journaled engine requires recovery"};
    }
    const auto next = engine_->next_sequence();
    if (is_mutating(command)) {
        const auto bytes = encode_record(next, command);
        try {
            append_(bytes);
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
    try {
        return engine_->execute_at(command, next);
    } catch (...) {
        failed_ = true;
        throw;
    }
}

}  // namespace exchange
