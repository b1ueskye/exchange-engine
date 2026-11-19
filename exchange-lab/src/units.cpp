#include "exchange/units.hpp"
#include "exchange/notional.hpp"

#include <limits>

namespace exchange {

namespace {

constexpr auto lo = std::numeric_limits<std::int64_t>::min();
constexpr auto hi = std::numeric_limits<std::int64_t>::max();

std::optional<Money> make_money(CurrencyId currency, std::optional<std::int64_t> units) {
    if (!units) {
        return std::nullopt;
    }
    return Money{currency, *units};
}

}

bool valid_symbol(const SymbolSpec& spec) {
    return spec.symbol.value() != 0 && spec.base.value() != 0 &&
           spec.quote.value() != 0 && spec.base != spec.quote &&
           spec.base_scale > 0 && spec.quote_scale > 0 &&
           spec.maker_fee_per_lot >= 0 &&
           spec.taker_fee_per_lot >= spec.maker_fee_per_lot;
}

std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b)) {
        return std::nullopt;
    }
    return a + b;
}

std::optional<std::int64_t> checked_subtract(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a < lo + b) || (b < 0 && a > hi + b)) {
        return std::nullopt;
    }
    return a - b;
}

std::optional<std::int64_t> checked_multiply(std::int64_t a, std::int64_t b) {
    if ((a > 0 && b > 0 && a > hi / b) ||
        (a > 0 && b < 0 && b < lo / a) ||
        (a < 0 && b > 0 && a < lo / b) ||
        (a < 0 && b < 0 && a < hi / b)) {
        return std::nullopt;
    }
    return a * b;
}

std::optional<Money> add_money(Money a, Money b) {
    if (a.currency != b.currency) {
        return std::nullopt;
    }
    return make_money(a.currency, checked_add(a.units, b.units));
}

std::optional<Money> base_amount(Quantity quantity, const SymbolSpec& spec) {
    if (!valid_symbol(spec) || quantity.value() < 0) {
        return std::nullopt;
    }
    return make_money(spec.base, checked_multiply(quantity.value(), spec.base_scale));
}

std::optional<Money> quote_notional(Quantity quantity, Price price, const SymbolSpec& spec) {
    if (!valid_symbol(spec)) {
        return std::nullopt;
    }
    return make_money(spec.quote, quote_notional(quantity.value(), price.value(), spec.quote_scale));
}

std::optional<Money> bid_hold(Quantity quantity, Price reserve, const SymbolSpec& spec) {
    const auto notional = quote_notional(quantity, reserve, spec);
    if (!notional) {
        return std::nullopt;
    }
    const auto fee = checked_multiply(quantity.value(), spec.taker_fee_per_lot);
    if (!fee) {
        return std::nullopt;
    }
    return add_money(*notional, Money{spec.quote, *fee});
}

std::optional<Money> budget_bid_hold(Quantity quantity, NotionalSteps budget,
                                     const SymbolSpec& spec) {
    if (!valid_symbol(spec) || quantity.value() <= 0 || budget.value() <= 0) {
        return std::nullopt;
    }
    const auto principal = checked_multiply(budget.value(), spec.quote_scale);
    const auto fee = checked_multiply(quantity.value(), spec.taker_fee_per_lot);
    if (!principal || !fee) {
        return std::nullopt;
    }
    return make_money(spec.quote, checked_add(*principal, *fee));
}

}