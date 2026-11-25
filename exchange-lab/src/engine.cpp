#include "exchange/engine.hpp"
#include "spot_update.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace exchange {

Status SymbolCatalog::add(const SymbolSpec& spec, BookKind kind,
                          std::size_t capacity) {
    if (!valid_symbol(spec)) {
        return Status::invalid_input;
    }
    if (markets_.contains(spec.symbol)) {
        return Status::symbol_exists;
    }
    markets_.try_emplace(spec.symbol,
                         Market{spec, std::make_unique<OrderBook>(spec.symbol, kind, capacity)});
    return Status::processed;
}

CommandResult Engine::dispatch(const RegisterSymbol& command, Seq) {
    return {catalog_.add(command.spec, book_kind_, pool_capacity_), {}};
}

CommandResult Engine::dispatch(const AddUser& command, Seq) {
    return {validate_command(command) ? ledger_.add_user(command.user)
                                      : Status::invalid_input,
            {}};
}

CommandResult Engine::dispatch(const AdjustBalance& command, Seq) {
    return {ledger_.adjust(command), {}};
}

CommandResult Engine::dispatch(const SuspendUser& command, Seq) {
    return {validate_command(command) ? ledger_.suspend(command.user)
                                      : Status::invalid_input,
            {}};
}

CommandResult Engine::dispatch(const ResumeUser& command, Seq) {
    return {validate_command(command) ? ledger_.resume(command.user)
                                      : Status::invalid_input,
            {}};
}

template <class T>
CommandResult Engine::dispatch(const T& command, Seq seq) {
    if (!validate_command(command)) {
        return {Status::invalid_input, {}};
    }
    const auto found = catalog_.markets_.find(command.key.symbol);
    if (found == catalog_.markets_.end()) {
        return {Status::unknown_symbol, {}};
    }

    auto& market = found->second;
    Ledger::SpotUpdate settlement{ledger_, command.key, market.spec};
    if constexpr (std::is_same_v<T, PlaceOrder>) {
        if (market.book->find_order(command.key.order)) {
            return {Status::duplicate_order, {}};
        }
        const auto frozen = settlement.freeze(command);
        if (frozen != Status::processed) {
            return {frozen, {}};
        }
    }

    auto plan = market.book->prepare(command, seq);
    if (plan.outcome.status != Status::processed) {
        return std::move(plan.outcome);
    }
    if constexpr (std::is_same_v<T, MoveOrder>) {
        const auto original = market.book->find_order(command.key.order);
        if (original && original->side == Side::ask) {
            const auto per_lot =
                quote_notional(Quantity{1}, command.new_price, market.spec);
            if (!per_lot || per_lot->units < market.spec.taker_fee_per_lot) {
                return {Status::invalid_input, {}};
            }
        }
    }
    if (!settlement.prepare(*market.book, plan.outcome.events, plan.after)) {
        return {Status::settlement_failed, {}};
    }

    // The book secures any remainder node and index entry before touching
    // existing orders. Once that succeeds, neither commit allocates.
    auto result = market.book->apply(std::move(plan));
    if (result.status == Status::processed) {
        settlement.commit();
    }
    return result;
}

Seq Engine::next_sequence() const {
    if (last_seq_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"command sequence exhausted"};
    }
    return Seq{last_seq_ + 1};
}

Result Engine::execute(const Command& command) {
    return execute_at(command, next_sequence());
}

Result Engine::execute_at(const Command& command, Seq seq) {
    if (seq.value() <= last_seq_) {
        throw std::invalid_argument{"sequence must advance"};
    }
    last_seq_ = seq.value();
    return std::visit([&](const auto& input) { return answer(input, seq); },
                      command);
}

std::optional<std::vector<RestingOrder>> Engine::book_orders(
    SymbolId symbol) const {
    const auto found = catalog_.markets_.find(symbol);
    if (found == catalog_.markets_.end()) {
        return std::nullopt;
    }
    return found->second.book->resting_orders();
}

CommandResult Engine::dispatch(const RegisterSymbols& command, Seq) {
    if (!validate_command(command)) {
        return {Status::invalid_input, {}};
    }
    SymbolCatalog staged;
    for (const auto& [id, market] : catalog_.markets_) {
        staged.markets_.try_emplace(
            id, SymbolCatalog::Market{market.spec, market.book->clone_order_book()});
    }
    for (const auto& spec : command.specs) {
        const auto status = staged.add(spec, book_kind_, pool_capacity_);
        if (status != Status::processed) {
            return {status, {}};
        }
    }
    catalog_.markets_.swap(staged.markets_);
    return {Status::processed, {}};
}

CommandResult Engine::dispatch(const AddUsers& command, Seq) {
    if (!validate_command(command)) {
        return {Status::invalid_input, {}};
    }
    auto staged = ledger_;
    for (const auto user : command.users) {
        const auto status = staged.add_user(user);
        if (status != Status::processed) {
            return {status, {}};
        }
    }
    ledger_.swap(staged);
    return {Status::processed, {}};
}

Result Engine::answer(const QueryUser& command, Seq seq) {
    if (!validate_command(command)) {
        return {seq, {Status::invalid_input, {}}};
    }
    auto report = ledger_.user_report(command.user);
    if (!report) {
        return {seq, {Status::unknown_user, {}}};
    }
    for (const auto& entry : catalog_.markets_) {
        for (const auto& order : entry.second.book->resting_orders()) {
            if (order.user == command.user) {
                report->orders.push_back(order);
            }
        }
    }
    return {seq, {Status::processed, {}}, std::move(*report)};
}

Result Engine::answer(const QueryTotals&, Seq seq) {
    return {seq, {Status::processed, {}}, ledger_.totals()};
}

Result Engine::answer(const QueryFingerprint&, Seq seq) {
    std::string canonical;
    ledger_.append_canonical(canonical);
    const auto number = [&canonical](auto value) {
        canonical += std::to_string(value);
        canonical += ',';
    };
    for (const auto& [id, market] : catalog_.markets_) {
        const auto& spec = market.spec;
        canonical += "symbol:";
        number(id.value());
        number(spec.base.value());
        number(spec.quote.value());
        number(spec.base_scale);
        number(spec.quote_scale);
        number(spec.maker_fee_per_lot);
        number(spec.taker_fee_per_lot);
        for (const auto& order : market.book->resting_orders()) {
            canonical += "order:";
            number(order.key.order.value());
            number(order.user.value());
            number(order.side == Side::bid);
            number(order.price.value());
            number(order.remaining.value());
            number(order.reserve_bid_price.has_value());
            number(order.reserve_bid_price ? order.reserve_bid_price->value()
                                           : 0);
            number(order.priority_seq.value());
            number(order.timestamp_ns);
        }
    }

    std::uint64_t hash = 14695981039346656037ULL;
    for (const char ch : canonical) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    return {seq, {Status::processed, {}}, StateFingerprint{hash}};
}

}  // namespace exchange
