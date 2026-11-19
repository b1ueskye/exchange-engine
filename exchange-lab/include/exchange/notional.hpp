#ifndef EXCHANGE_NOTIONAL_HPP
#define EXCHANGE_NOTIONAL_HPP

#include <cstdint>
#include <optional>

namespace exchange {

std::optional<std::int64_t> quote_notional(std::int64_t quantity,
                                           std::int64_t price,
                                           std::int64_t quote_scale);

}

#endif