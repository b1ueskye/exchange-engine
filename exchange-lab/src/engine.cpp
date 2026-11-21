#include "exchange/engine.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>

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
    // todo : consider a more efficient way to stage the order book changes
    // without copying all resting orders
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

    if (result.status == Status::processed) {
        market.book.swap(staged);
    }
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