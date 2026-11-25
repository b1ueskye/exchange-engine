#include "exchange/persistence.hpp"

#include <fstream>
#include "exchange/futures_risk.hpp"
#include <limits>
#include <algorithm>
#include <utility>

namespace exchange {
namespace {

using wire::Reader;
using wire::Writer;
constexpr std::uint32_t snapshot_magic = 0x31504e53;
constexpr unsigned snapshot_version = 2;
constexpr std::uint64_t max_total_slots = 65'536;

bool valid_layout(BookKind kind, std::uint64_t capacity, std::size_t markets) {
    return capacity <= wire::max_batch &&
           (kind != BookKind::pooled || markets <= max_total_slots / (capacity + 1));
}

void count(Writer& out, std::size_t size) {
    if (size > wire::max_batch) {
        throw std::length_error{"snapshot collection too large"};
    }
    out.u(size, 4);
}

std::size_t count(Reader& in) {
    const auto size = in.u(4);
    if (size > wire::max_batch) {
        throw std::runtime_error{"snapshot collection too large"};
    }
    return static_cast<std::size_t>(size);
}

CurrencyId currency(Reader& in) {
    const CurrencyId id{static_cast<std::uint32_t>(in.u(4))};
    if (id.value() == 0) {
        throw std::runtime_error{"zero currency"};
    }
    return id;
}

template<class Map>
void amounts(Writer& out, const Map& map) {
    count(out, map.size());
    for (const auto& [id, units] : map) {
        out.u(id.value(), 4);
        out.i(units);
    }
}

template<class Map>
void amounts(Reader& in, Map& map, bool nonnegative = true) {
    const auto size = count(in);
    for (std::size_t i = 0; i < size; ++i) {
        const auto id = currency(in);
        const auto units = in.i();
        if ((nonnegative && units < 0) || !map.try_emplace(id, units).second) {
            throw std::runtime_error{"invalid balance row"};
        }
    }
}

void write_order(Writer& out, const RestingOrder& order) {
    out.u(order.key.order.value());
    out.u(order.user.value());
    out.u(order.side == Side::bid, 1);
    out.i(order.price.value());
    out.i(order.remaining.value());
    out.u(order.reserve_bid_price.has_value(), 1);
    if (order.reserve_bid_price) {
        out.i(order.reserve_bid_price->value());
    }
    out.u(order.priority_seq.value());
    out.i(order.timestamp_ns);
}

RestingOrder read_order(Reader& in, SymbolId symbol, Seq checkpoint) {
    const OrderId id{in.u()};
    const UserId user{in.u()};
    const auto side = in.flag() ? Side::bid : Side::ask;
    const Price price{in.i()};
    const Quantity remaining{in.i()};
    std::optional<Price> reserve;
    if (in.flag()) {
        reserve = Price{in.i()};
    }
    const Seq priority{in.u()};
    if (priority > checkpoint) {
        throw std::runtime_error{"priority beyond checkpoint"};
    }
    return {{symbol, id}, user, side, price, remaining, reserve, priority, in.i()};
}

}

Bytes SnapshotStore::encode(const Engine& engine) {
    if (!valid_layout(engine.book_kind_, engine.pool_capacity_, engine.catalog_.markets_.size())) {
        throw std::length_error{"snapshot pool capacity too large"};
    }
    Writer payload;
    payload.u(engine.last_sequence().value());
    payload.u(engine.book_kind_ == BookKind::pooled, 1);
    payload.u(engine.pool_capacity_);
    payload.u(engine.send_l2_, 1);
    count(payload, engine.catalog_.markets_.size());
    for (const auto& [id, market] : engine.catalog_.markets_) {
        wire::write_spec(payload, market.spec);
        const auto orders = market.book->resting_orders();
        count(payload, orders.size());
        for (const auto& order : orders) {
            write_order(payload, order);
        }
    }
    const auto& ledger = engine.ledger_;
    count(payload, ledger.accounts_.size());
    for (const auto& [user, account] : ledger.accounts_) {
        payload.u(user.value());
        payload.u(account.last_funding);
        payload.u(account.suspended, 1);
        amounts(payload, account.balances);
        count(payload, account.positions.size());
        for (const auto& [symbol, p] : account.positions) {
            payload.u(symbol.value(), 4); payload.i(p.signed_quantity); payload.i(p.cost_sum);
            payload.i(p.profit); payload.i(p.pending_buy); payload.i(p.pending_sell);
        }
        payload.u(account.position_order.one(), 1);
        count(payload, account.position_order.slots().size());
        for (const auto key : account.position_order.slots()) { payload.u(key, 4); }

    }
    count(payload, ledger.holds_.size());
    for (const auto& [key, hold] : ledger.holds_) {
        payload.u(key.first.value(), 4);
        payload.u(key.second.value());
        payload.u(hold.user.value());
        payload.u(hold.money.currency.value(), 4);
        payload.i(hold.money.units);
    }
    amounts(payload, ledger.fees_);
    amounts(payload, ledger.net_deposits_);
    count(payload, ledger.last_prices_.size());
    for (const auto& [symbol, p] : ledger.last_prices_) { payload.u(symbol.value(), 4); payload.i(p.ask); payload.i(p.bid); }
    if (payload.data.size() > wire::max_payload) {
        throw std::length_error{"snapshot payload too large"};
    }

    Writer out;
    out.u(snapshot_magic, 4);
    out.u(snapshot_version, 1);
    out.u(payload.data.size(), 4);
    out.data.insert(out.data.end(), payload.data.begin(), payload.data.end());
    out.u(wire::checksum(out.data), 4);
    return std::move(out.data);
}

std::unique_ptr<Engine> SnapshotStore::decode(std::span<const std::byte> bytes) {
    Reader frame{bytes};
    if (frame.u(4) != snapshot_magic) { throw std::runtime_error{"invalid snapshot magic"}; }
    const auto version = static_cast<unsigned>(frame.u(1));
    if (version != 1 && version != snapshot_version) {
        throw std::runtime_error{"unsupported snapshot header"};
    }
    const auto length = frame.u(4);
    if (length > wire::max_payload || frame.left() != length + 4) {
        throw std::runtime_error{"invalid snapshot length"};
    }
    Reader in{frame.take(static_cast<std::size_t>(length))};
    const auto covered = bytes.first(frame.pos);
    if (frame.u(4) != wire::checksum(covered)) {
        throw std::runtime_error{"snapshot checksum mismatch"};
    }
    const Seq seq{in.u()};
    const auto kind = in.flag() ? BookKind::pooled : BookKind::list;
    const auto capacity = in.u();
    const bool send_l2 = version >= 2 ? in.flag() : true;
    const auto markets = count(in);
    if (!valid_layout(kind, capacity, markets)) {
        throw std::runtime_error{"snapshot pool capacity too large"};
    }
    auto engine = std::make_unique<Engine>(kind, static_cast<std::size_t>(capacity), send_l2);
    engine->last_seq_ = seq.value();

    for (std::size_t m = 0; m < markets; ++m) {
        const auto spec = wire::read_spec(in, version);
        const auto size = count(in);
        std::vector<RestingOrder> orders;
        orders.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            orders.push_back(read_order(in, spec.symbol, seq));
        }
        if (!valid_symbol(spec) || engine->catalog_.markets_.contains(spec.symbol)) {
            throw std::runtime_error{"invalid snapshot market"};
        }
        auto book = OrderBook::from_resting(spec.symbol, orders, kind,
                                            static_cast<std::size_t>(capacity));
        engine->catalog_.markets_.try_emplace(spec.symbol,
            SymbolCatalog::Market{spec, std::move(book)});
    }

    auto& ledger = engine->ledger_;
    const auto users = count(in);
    for (std::size_t i = 0; i < users; ++i) {
        const UserId user{in.u()};
        const auto funding = in.u();
        const auto suspended = in.flag();
        if (ledger.add_user(user) != Status::processed) {
            throw std::runtime_error{"invalid snapshot user"};
        }
        auto& account = ledger.accounts_.at(user);
        account.last_funding = funding;
        account.suspended = suspended;
        amounts(in, account.balances, version == 1);
        if (version >= 2) {
            const auto positions = count(in);
            for (std::size_t j = 0; j < positions; ++j) {
                const SymbolId symbol{static_cast<std::uint32_t>(in.u(4))};
                const MarginPosition p{in.i(), in.i(), in.i(), in.i(), in.i()};
                const auto market = engine->catalog_.markets_.find(symbol);
                if (market == engine->catalog_.markets_.end() || market->second.spec.type != SymbolType::futures ||
                    p.signed_quantity == std::numeric_limits<std::int64_t>::min() || p.pending_buy < 0 || p.pending_sell < 0 ||
                    (p.signed_quantity == 0 && p.cost_sum != 0) || p.empty() || account.suspended ||
                    !account.positions.try_emplace(symbol, p).second) { throw std::runtime_error{"invalid margin position"}; }
            }
            const bool one = in.flag();
            std::vector<std::uint32_t> slots(count(in));
            for (auto& key : slots) { key = static_cast<std::uint32_t>(in.u(4)); }
            account.position_order = PositionTraversalOrder::restore(std::move(slots), one);
            auto keys = account.position_order.keys();
            std::sort(keys.begin(), keys.end());
            std::vector<std::uint32_t> expected;
            for (const auto& [symbol, p] : account.positions) { expected.push_back(symbol.value()); }
            if (keys != expected) { throw std::runtime_error{"position table differs"}; }
        }
        if (suspended) {
            for (const auto& entry : account.balances) {
                if (entry.second != 0) {
                    throw std::runtime_error{"suspended user has funds"};
                }
            }
        }
    }
    const auto holds = count(in);
    for (std::size_t i = 0; i < holds; ++i) {
        const SymbolId symbol{static_cast<std::uint32_t>(in.u(4))};
        const OrderId order{in.u()};
        const UserId user{in.u()};
        const Money money{currency(in), in.i()};
        if (money.units <= 0 || !ledger.holds_.try_emplace(
                Ledger::HoldKey{symbol, order}, Ledger::Hold{user, money}).second) {
            throw std::runtime_error{"invalid snapshot hold"};
        }
    }
    amounts(in, ledger.fees_);
    amounts(in, ledger.net_deposits_, version == 1);
    if (version >= 2) {
        const auto prices = count(in);
        for (std::size_t j = 0; j < prices; ++j) {
            const SymbolId symbol{static_cast<std::uint32_t>(in.u(4))};
            const LastPrice p{in.i(), in.i()};
            if (!engine->catalog_.markets_.contains(symbol) || p.ask <= 0 || p.bid < 0 ||
                !ledger.last_prices_.try_emplace(symbol, p).second) { throw std::runtime_error{"invalid last price cache"}; }
        }
    }
    in.end();

    std::size_t checked = 0;
    for (const auto& [id, market] : engine->catalog_.markets_) {
        for (const auto& order : market.book->resting_orders()) {
            const auto account = ledger.accounts_.find(order.user);
            if (market.spec.type == SymbolType::futures) {
                if (account == ledger.accounts_.end() || account->second.suspended ||
                    !account->second.positions.contains(id)) { throw std::runtime_error{"missing position owner"}; }
                continue;
            }
            const auto hold = ledger.holds_.find({id, order.key.order});
            const auto expected = order.side == Side::bid
                ? bid_hold(order.remaining, *order.reserve_bid_price, market.spec)
                : base_amount(order.remaining, market.spec);
            if (account == ledger.accounts_.end() || account->second.suspended ||
                hold == ledger.holds_.end() || hold->second.user != order.user ||
                !expected || hold->second.money != *expected) {
                throw std::runtime_error{"snapshot order and hold differ"};
            }
            ++checked;
        }
    }
    if (checked != ledger.holds_.size()) {
        throw std::runtime_error{"orphan hold"};
    }
    for (const auto& [user, a] : ledger.accounts_) {
        for (const auto& [symbol, p] : a.positions) {
            std::int64_t buy{}, sell{};
            for (const auto& order : engine->catalog_.markets_.at(symbol).book->resting_orders()) {
                if (order.user == user) {
                    auto& total = order.side == Side::bid ? buy : sell;
                    const auto next = checked_add(total, order.remaining.value());
                    if (!next) { throw std::runtime_error{"pending overflow"}; }
                    total = *next;
                }
            }
            if (buy != p.pending_buy || sell != p.pending_sell) { throw std::runtime_error{"snapshot pending differs"}; }
        }
    }
    auto totals = ledger.totals();
    FuturesRisk::totals(ledger, totals, [&](SymbolId id) -> const SymbolSpec& { return engine->catalog_.markets_.at(id).spec; });
    for (const auto& oi : totals.open_interest) { if (oi.long_volume != oi.short_volume) { throw std::runtime_error{"snapshot open interest"}; } }
    for (const auto& row : totals.currencies) {
        if (!row.conserved()) {
            throw std::runtime_error{"snapshot totals do not conserve"};
        }
    }
    return engine;
}

void SnapshotStore::publish(const std::filesystem::path& destination, const Engine& engine) {
    const auto bytes = encode(engine);
    auto temporary = destination;
    temporary += ".tmp";
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    write_binary(output, bytes);
    output.close();
    if (!output) {
        throw std::runtime_error{"could not close temporary snapshot"};
    }
    std::filesystem::rename(temporary, destination);
}

JournaledEngine::JournaledEngine(Append append, std::unique_ptr<Engine> recovered)
    : engine_(std::move(recovered)), append_(std::move(append)) {
    if (!engine_) {
        throw std::invalid_argument{"missing recovered engine"};
    }
}

}
