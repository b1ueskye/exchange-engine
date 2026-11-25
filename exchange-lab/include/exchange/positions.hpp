#ifndef EXCHANGE_POSITIONS_HPP
#define EXCHANGE_POSITIONS_HPP
#include "exchange/units.hpp"
#include <limits>

namespace exchange {
struct MarginPosition {
    std::int64_t signed_quantity{};
    std::int64_t cost_sum{};
    std::int64_t profit{};
    std::int64_t pending_buy{};
    std::int64_t pending_sell{};
    bool empty() const {
        return signed_quantity == 0 && pending_buy == 0 && pending_sell == 0;
    }
    bool operator==(const MarginPosition&) const = default;
};
struct LastPrice {
    std::int64_t ask = std::numeric_limits<std::int64_t>::max();
    std::int64_t bid{};
    bool operator==(const LastPrice&) const = default;
};
struct PositionReport {
    SymbolId symbol;
    CurrencyId currency;
    MarginPosition position;
    std::int64_t estimated_profit;
    std::int64_t required_margin;
    bool operator==(const PositionReport&) const = default;
};
struct OpenInterest {
    SymbolId symbol;
    std::int64_t long_volume{};
    std::int64_t short_volume{};
    bool operator==(const OpenInterest&) const = default;
};
}  // namespace exchange
#endif
