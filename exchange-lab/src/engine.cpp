#include "exchange/engine.hpp"
#include "exchange/futures_risk.hpp"

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
    markets_.try_emplace(
        spec.symbol,
        Market{spec, make_book(kind, spec.symbol, capacity, spec.type)});
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
    const auto found = catalog_.markets_.find(command.key.symbol);
    if (found == catalog_.markets_.end()) {
        return {Status::unknown_symbol, {}};
    }
    auto& market = found->second;
    if (!validate_command(command, market.spec.type)) {
        return {Status::invalid_input, {}};
    }
    const SpecLookup lookup = [&](SymbolId id) -> const SymbolSpec& {
        return catalog_.markets_.at(id).spec;
    };
    const bool futures = market.spec.type == SymbolType::futures;
    auto staged_ledger = ledger_;
    try {
        if constexpr (std::is_same_v<T, PlaceOrder>) {
            if (market.book->find_order(command.key.order)) {
                return {Status::duplicate_order, {}};
            }
            const auto status =
                futures ? FuturesRisk::reserve(staged_ledger, command,
                                               market.spec, lookup)
                        : SpotRisk::reserve(staged_ledger, command, market.spec,
                                            lookup);
            if (status != Status::processed) {
                if (futures && status == Status::insufficient_funds) {
                    ledger_.swap(staged_ledger);
                }
                return {status, {}};
            }
        }
        auto staged = market.book->clone();
        auto result = [&] {
            if constexpr (std::is_same_v<T, PlaceOrder>) {
                return staged->place(command, seq);
            } else if constexpr (std::is_same_v<T, CancelOrder>) {
                return staged->cancel(command);
            } else if constexpr (std::is_same_v<T, ReduceOrder>) {
                return staged->reduce(command);
            } else {
                return staged->move(command, seq);
            }
        }();
        if (result.status != Status::processed) {
            return result;
        }
        if constexpr (std::is_same_v<T, MoveOrder>) {
            const auto original = market.book->find_order(command.key.order);
            if (!futures && original && original->side == Side::ask) {
                const auto amount =
                    quote_notional(Quantity{1}, command.new_price, market.spec);
                if (!amount || amount->units < market.spec.taker_fee_per_lot) {
                    return {Status::invalid_input, {}};
                }
            }
        }
        const PlaceOrder* placed = nullptr;
        if constexpr (std::is_same_v<T, PlaceOrder>) {
            placed = &command;
        }
        const auto settled =
            futures ? FuturesRisk::settle(staged_ledger, placed, command.key,
                                          result.events, *staged, market.spec)
                    : SpotRisk::settle(staged_ledger, command.key,
                                       result.events, *staged, market.spec);
        if (!settled) {
            return {Status::settlement_failed, {}};
        }
        if (send_l2_) {
            FuturesRisk::refresh(staged_ledger, market.spec.symbol, *staged);
        }
        market.book.swap(staged);
        ledger_.swap(staged_ledger);
        return result;
    } catch (const std::overflow_error&) {
        return {Status::settlement_failed, {}};
    }
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
            id, SymbolCatalog::Market{market.spec, market.book->clone()});
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
    FuturesRisk::reports(ledger_, *report,
                         [&](SymbolId id) -> const SymbolSpec& {
                             return catalog_.markets_.at(id).spec;
                         });
    return {seq, {Status::processed, {}}, std::move(*report)};
}

Result Engine::answer(const QueryTotals&, Seq seq) {
    auto report = ledger_.totals();
    FuturesRisk::totals(ledger_, report, [&](SymbolId id) -> const SymbolSpec& {
        return catalog_.markets_.at(id).spec;
    });
    return {seq, {Status::processed, {}}, std::move(report)};
}

Result Engine::answer(const QueryFingerprint&, Seq seq) {
    std::string canonical;
    ledger_.append_canonical(canonical);
    canonical += send_l2_ ? "l2=1;" : "l2=0;";
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
        number(static_cast<int>(spec.type));
        number(spec.margin_buy);
        number(spec.margin_sell);
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
