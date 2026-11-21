#ifndef EXCHANGE_ORDER_BOOK_HPP
#define EXCHANGE_ORDER_BOOK_HPP

#include "exchange/model.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>
#include<memory>

namespace exchange {

struct PriceLevel {
    Price price;
    Quantity total_remaining;
    std::size_t order_count;
    bool operator==(const PriceLevel&) const = default;
};

struct L2Snapshot {
    std::vector<PriceLevel> asks;
    std::vector<PriceLevel> bids;
    bool operator==(const L2Snapshot&) const = default;
};

struct OrderBookTestAccess;

class OrderBook {
public:
    explicit OrderBook(SymbolId symbol);
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<RestingOrder> find_order(OrderId id) const;
    bool validate() const;

    L2Snapshot snapshot(std::size_t depth) const;
    std::vector<RestingOrder> resting_orders() const;

    CommandResult place(const PlaceOrder& order, Seq seq);
    CommandResult cancel(const CancelOrder& command);
    CommandResult reduce(const ReduceOrder& command);
    CommandResult move(const MoveOrder& command, Seq seq);

        static std::unique_ptr<OrderBook> from_resting(
            SymbolId symbol, const std::vector<RestingOrder>& orders);

private:
    friend struct OrderBookTestAccess;

    struct Level {
        std::list<RestingOrder> fifo;
        std::int64_t total_remaining{};
    };

    struct OrderLocation {
        Side side;
        Price price;
        std::list<RestingOrder>::iterator node;
    };

    struct OrderIdHash {
        std::size_t operator()(OrderId id) const noexcept {
            return std::hash<std::uint64_t>{}(id.value());
        }
    };

    using AskLevels = std::map<Price, Level, std::less<Price>>;
    using BidLevels = std::map<Price, Level, std::greater<Price>>;
    using Index = std::unordered_map<OrderId, OrderLocation, OrderIdHash>;
    using BeforeIndexInsert = void (*)();

    bool valid_resting(const RestingOrder& order) const;
    bool crosses(const RestingOrder& order) const;
    Status add_resting(const RestingOrder& order,
                       BeforeIndexInsert before_index = nullptr);
    std::optional<RestingOrder> remove_resting(OrderId id, UserId owner);

    template <class Levels>
    Status append_to(Levels& levels, const RestingOrder& order,
                     BeforeIndexInsert before_index);

    template <class Levels>
    RestingOrder remove_from(Levels& levels, Index::iterator indexed);

    struct MatchPlan {
        Quantity remaining;
        std::vector<Event> events;
    };

    MatchPlan plan_match(const PlaceOrder& order,
                         std::optional<Price> limit) const;
    void apply_trades(const std::vector<Event>& events);
    void decrease_resting(Index::iterator indexed, Quantity amount);

    template <class Levels>
    MatchPlan plan_against(const Levels& levels, const PlaceOrder& order,
                           std::optional<Price> limit) const;

    template <class Levels>
    void decrease_in(Levels& levels, Index::iterator indexed, Quantity amount);

    using BudgetTrial = std::variant<NotionalSteps, RejectReason>;

    template <class Levels>
    BudgetTrial trial_budget(const Levels& levels, Quantity quantity) const;

    CommandResult place_budget(const PlaceOrder& order, BudgetLimit budget);

    CommandResult reduce_owned(OrderKey key, UserId user,
                               std::optional<Quantity> requested);

    template <class Levels>
    Status replace_resting(Levels& levels, Index::iterator indexed,
                           const RestingOrder& order);

    SymbolId symbol_;
    AskLevels asks_;
    BidLevels bids_;
    Index index_;
};

}  // namespace exchange

#endif