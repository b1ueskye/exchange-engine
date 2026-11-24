#ifndef EXCHANGE_MODEL_HPP
#define EXCHANGE_MODEL_HPP

#include "exchange/order_draft.hpp"
#include "exchange/units.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace exchange {

struct OrderKey {
    SymbolId symbol;
    OrderId order;
    bool operator==(const OrderKey &) const = default;
};

enum class LimitPolicy { gtc, ioc };

struct LimitPrice {
    Price price;
    LimitPolicy policy;
    std::optional<Price> reserve_bid_price;
};

struct BudgetLimit {
    NotionalSteps total;
};

using OrderConstraint = std::variant<LimitPrice, BudgetLimit>;

struct PlaceOrder {
    OrderKey key;
    UserId user;
    Side side;
    Quantity quantity;
    OrderConstraint constraint;
    std::int64_t timestamp_ns{};
};

struct CancelOrder {
    OrderKey key;
    UserId user;
};

struct ReduceOrder {
    OrderKey key;
    UserId user;
    Quantity quantity;
};

struct MoveOrder {
    OrderKey key;
    UserId user;
    Price new_price;
};

struct RegisterSymbol {
    SymbolSpec spec;
};

struct AddUser {
    UserId user;
};
struct AdjustBalance {
    UserId user;
    Money amount;
    std::uint64_t funding_id;
};
struct SuspendUser {
    UserId user;
};
struct ResumeUser {
    UserId user;
};

struct RegisterSymbols { std::vector<SymbolSpec> specs; };
struct AddUsers { std::vector<UserId> users; };
struct QueryUser { UserId user; };
struct QueryTotals {};
struct QueryFingerprint {};

using Command = std::variant<PlaceOrder, CancelOrder, ReduceOrder, MoveOrder, RegisterSymbol,
                             AddUser, AdjustBalance, SuspendUser, ResumeUser,
                             RegisterSymbols, AddUsers, QueryUser, QueryTotals, QueryFingerprint>;
struct RestingOrder {
    OrderKey key;
    UserId user;
    Side side;
    Price price;
    Quantity remaining;
    std::optional<Price> reserve_bid_price;
    Seq priority_seq;
    std::int64_t timestamp_ns{};
    bool operator==(const RestingOrder &) const = default;
};

enum class Status {
    processed,
    invalid_input,
    duplicate_order,
    unknown_symbol,
    unknown_order,
    insufficient_funds,
    price_over_reserve,
    symbol_exists,
    unknown_user,
    user_suspended,
    user_exists,
    account_not_empty,
    duplicate_funding,
    stale_funding,
    settlement_failed,
};

enum class RejectReason {
    ioc_remainder,
    insufficient_liquidity,
    budget_limit,
    arithmetic_limit
};

struct Trade {
    OrderKey maker;
    UserId maker_user;
    OrderKey taker;
    UserId taker_user;
    Side taker_side;
    Price price;
    Quantity quantity;
    bool maker_filled;
    bool taker_filled;
    bool operator==(const Trade &) const = default;
};

struct Reduce {
    OrderKey key;
    UserId user;
    Side side;
    Price price;
    Quantity quantity;
    bool removed;
    bool operator==(const Reduce &) const = default;
};

struct RejectRemainder {
    OrderKey key;
    UserId user;
    Quantity quantity;
    RejectReason reason;
    bool operator==(const RejectRemainder &) const = default;
};

using Event = std::variant<Trade, Reduce, RejectRemainder>;

struct CommandResult {
    Status status;
    std::vector<Event> events;
    bool operator==(const CommandResult &) const = default;
};

struct FrozenOrder {
    OrderKey key;
    Money amount;
    bool operator==(const FrozenOrder&) const = default;
};

struct UserReport {
    UserId user;
    bool suspended;
    std::uint64_t last_funding;
    std::vector<Money> available;
    std::vector<FrozenOrder> frozen;
    std::vector<RestingOrder> orders;
};

struct CurrencyTotals {
    CurrencyId currency;
    std::int64_t available{};
    std::int64_t frozen{};
    std::int64_t fees{};
    std::int64_t net_deposits{};
    bool conserved() const;
};

struct TotalsReport {
    std::vector<CurrencyTotals> currencies;
};

struct StateFingerprint {
    std::uint64_t value;
};

using Report = std::variant<std::monostate, UserReport, TotalsReport, StateFingerprint>;

struct Result {
    Seq seq;
    CommandResult outcome;
    Report report{};
};

bool validate_command(const Command &command);
std::string describe_event(const Event &event);
std::string describe_result(const Result &result);

}  // namespace exchange

#endif
