#ifndef EXCHANGE_UNITS_HPP
#define EXCHANGE_UNITS_HPP

#include <compare>
#include <cstdint>
#include <optional>

namespace exchange {

template<class Tag, class Rep = std::int64_t>
class Scalar {
public:
    explicit constexpr Scalar(Rep value) : value_{value} {}
    constexpr Rep value() const { return value_; }

    friend auto operator<=>(const Scalar&, const Scalar&) = default;

private:
    Rep value_;
};

struct PriceTag;
struct QuantityTag;
struct NotionalStepsTag;
struct CurrencyTag;
struct SymbolTag;
struct OrderTag;
struct UserTag;
struct SequenceTag;

using Price = Scalar<PriceTag>;
using Quantity = Scalar<QuantityTag>;
using NotionalSteps = Scalar<NotionalStepsTag>;
using CurrencyId = Scalar<CurrencyTag, std::uint32_t>;
using SymbolId = Scalar<SymbolTag, std::uint32_t>;
using OrderId = Scalar<OrderTag, std::uint64_t>;
using UserId = Scalar<UserTag, std::uint64_t>;
using Seq = Scalar<SequenceTag, std::uint64_t>;

struct Money {
    CurrencyId currency;
    std::int64_t units;
    bool operator==(const Money&) const = default;
};

struct SymbolSpec {
    SymbolId symbol;
    CurrencyId base;
    CurrencyId quote;
    std::int64_t base_scale;
    std::int64_t quote_scale;
    std::int64_t maker_fee_per_lot;
    std::int64_t taker_fee_per_lot;
};

bool valid_symbol(const SymbolSpec& spec);
std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b);
std::optional<std::int64_t> checked_subtract(std::int64_t a, std::int64_t b);
std::optional<std::int64_t> checked_multiply(std::int64_t a, std::int64_t b);
std::optional<Money> add_money(Money a, Money b);
std::optional<Money> base_amount(Quantity quantity, const SymbolSpec& spec);
std::optional<Money> quote_notional(Quantity quantity, Price price, const SymbolSpec& spec);
std::optional<Money> bid_hold(Quantity quantity, Price reserve, const SymbolSpec& spec);
std::optional<Money> budget_bid_hold(Quantity quantity, NotionalSteps budget,
                                     const SymbolSpec& spec);

}

#endif