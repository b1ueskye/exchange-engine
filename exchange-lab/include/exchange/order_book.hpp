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

namespace exchange {

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
    Status add_resting(
        const RestingOrder& order,
        BeforeIndexInsert before_index = nullptr);
    std::optional<RestingOrder> remove_resting(OrderId id, UserId owner);

    template <class Levels>
    Status append_to(
        Levels& levels,
        const RestingOrder& order,
        BeforeIndexInsert before_index);

    template <class Levels>
    RestingOrder remove_from(Levels& levels, Index::iterator indexed);

    SymbolId symbol_;
    AskLevels asks_;
    BidLevels bids_;
    Index index_;
};

}

#endif