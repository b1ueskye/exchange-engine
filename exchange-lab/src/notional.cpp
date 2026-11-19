#include "exchange/notional.hpp"

#include <limits>

namespace exchange {

std::optional<std::int64_t> quote_notional(
    std::int64_t quantity,
    std::int64_t price,
    std::int64_t quote_scale) {
    if (quantity < 0 || price <= 0 || quote_scale <= 0) {
        return std::nullopt;
    }
    const std::int64_t max_value = std::numeric_limits<std::int64_t>::max();
    if (quantity > max_value / price) {
        return std::nullopt;
    }
    std::int64_t lots_times_steps = quantity * price;
    if (lots_times_steps > max_value / quote_scale) {
        return std::nullopt;
    }
    return lots_times_steps * quote_scale;
}

} // namespace exchange