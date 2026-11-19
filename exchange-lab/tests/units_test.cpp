#include "exchange/units.hpp"
#include "test_checks.hpp"

#include <limits>
#include <type_traits>

int main() {
    using namespace exchange;
    static_assert(!std::is_convertible_v<Price, Quantity>);
    static_assert(!std::is_convertible_v<NotionalSteps, Price>);
    static_assert(!std::is_convertible_v<int, Price>);

    TestChecks checks;
    const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 10, 10, 1, 3};
    const auto base = base_amount(Quantity{3}, spec);
    const auto quote = quote_notional(Quantity{3}, Price{102}, spec);
    const auto hold = bid_hold(Quantity{3}, Price{105}, spec);
    const auto budget = budget_bid_hold(Quantity{3}, NotionalSteps{306}, spec);
    checks.require(base && *base == Money{spec.base, 30}, "base units");
    checks.require(quote && *quote == Money{spec.quote, 3060}, "quote units");
    checks.require(hold && *hold == Money{spec.quote, 3159}, "regular buy hold");
    checks.require(budget && *budget == Money{spec.quote, 3069}, "budget buy hold");
    checks.require(!add_money(Money{spec.base, 1}, Money{spec.quote, 1}), "currency mismatch");
    checks.require(add_money(Money{spec.quote, 12}, Money{spec.quote, -5}) ==
                   Money{spec.quote, 7}, "signed adjustment");

    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    checks.require(checked_add(hi, 0) == hi && !checked_add(hi, 1), "upper addition edge");
    checks.require(checked_add(lo, 0) == lo && !checked_add(lo, -1), "lower addition edge");
    checks.require(!checked_subtract(lo, 1) && !checked_subtract(hi, -1),
                   "subtraction overflow in both directions");
    checks.require(checked_subtract(lo, lo) == 0 && !checked_subtract(0, lo),
                   "subtraction without negating min");
    checks.require(checked_multiply(hi, 1) == hi && !checked_multiply(hi, 2),
                   "positive multiplication edge");
    checks.require(checked_multiply(lo, 1) == lo && checked_multiply(1, lo) == lo &&
                   !checked_multiply(lo, 2) && !checked_multiply(2, lo),
                   "negative multiplication edge");
    checks.require(checked_multiply(-2, -3) == 6 && !checked_multiply(lo, -1) &&
                   !checked_multiply(-1, lo) && checked_multiply(lo, 0) == 0,
                   "both negative and zero factors");

    auto boundary = spec;
    boundary.quote_scale = 1;
    boundary.taker_fee_per_lot = 1;
    checks.require(quote_notional(Quantity{1}, Price{hi}, boundary) ==
                   Money{spec.quote, hi}, "max quote amount");
    checks.require(bid_hold(Quantity{1}, Price{hi - 1}, boundary) ==
                   Money{spec.quote, hi}, "max buy hold");
    checks.require(!bid_hold(Quantity{1}, Price{hi}, boundary), "fee makes hold overflow");
    checks.require(bid_hold(Quantity{0}, Price{hi}, spec) ==
                   Money{spec.quote, 0}, "zero ordinary hold");
    checks.require(!quote_notional(Quantity{1}, Price{0}, spec) &&
                   !base_amount(Quantity{-1}, spec), "invalid price or quantity");
    checks.require(!budget_bid_hold(Quantity{3}, NotionalSteps{0}, spec),
                   "invalid budget");
    boundary.quote_scale = 0;
    checks.require(!base_amount(Quantity{1}, boundary), "invalid symbol scale");
    return checks.exit_code();
}