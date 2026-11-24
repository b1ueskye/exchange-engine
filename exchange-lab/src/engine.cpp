#include "exchange/engine.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace exchange {

Status SymbolCatalog::add(const SymbolSpec& spec) {
    if (!valid_symbol(spec)) {
        return Status::invalid_input;
    }
    if (markets_.contains(spec.symbol)) {
        return Status::symbol_exists;
    }
    markets_.try_emplace(
        spec.symbol, Market{spec, std::make_unique<OrderBook>(spec.symbol)});
    return Status::processed;
}

CommandResult Engine::dispatch(const RegisterSymbol& command, Seq) {
    return {catalog_.add(command.spec), {}};
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
    auto staged_ledger = ledger_;
    if constexpr (std::is_same_v<T, PlaceOrder>) {
        if (market.book->find_order(command.key.order)) {
            return {Status::duplicate_order, {}};
        }
        const auto frozen = staged_ledger.freeze(command, market.spec);
        if (frozen != Status::processed) {
            return {frozen, {}};
        }
    }

    auto staged = OrderBook::from_resting(market.spec.symbol,
                                          market.book->resting_orders());
    auto result = [&] {
        if constexpr (std::is_same_v<T, PlaceOrder>) {
            return staged->place(command, seq);
        } else if constexpr (std::is_same_v<T, CancelOrder>) {
            return staged->cancel(command);
        } else if constexpr (std::is_same_v<T, ReduceOrder>) {
            return staged->reduce(command);
        } else {
            static_assert(std::is_same_v<T, MoveOrder>);
            return staged->move(command, seq);
        }
    }();
    if (result.status != Status::processed) {
        return result;
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
    if (!staged_ledger.apply_spot(command.key, result.events, *staged,
                                  market.spec)) {
        return {Status::settlement_failed, {}};
    }
    // TODO: consider a more efficient swap or move operation for the order book
    // and ledger
    market.book.swap(staged);
    ledger_.swap(staged_ledger);
    return result;
}

Result Engine::execute(const Command& command) {
    if (last_seq_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"command sequence exhausted"};
    }
    const Seq seq{++last_seq_};
    return {seq,
            std::visit([&](const auto& input) { return dispatch(input, seq); },
                       command)};
}

std::optional<std::vector<RestingOrder>> Engine::book_orders(
    SymbolId symbol) const {
    const auto found = catalog_.markets_.find(symbol);
    if (found == catalog_.markets_.end()) {
        return std::nullopt;
    }
    return found->second.book->resting_orders();
}

}  // namespace exchange
