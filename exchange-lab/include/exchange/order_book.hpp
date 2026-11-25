#ifndef EXCHANGE_ORDER_BOOK_HPP
#define EXCHANGE_ORDER_BOOK_HPP

#include "exchange/model.hpp"
#include "exchange/order_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>
#include <memory>

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

enum class BookKind { list, pooled };

class Book {
public:
    virtual ~Book() = default;
    virtual std::unique_ptr<Book> clone() const = 0;
    virtual CommandResult place(const PlaceOrder&, Seq) = 0;
    virtual CommandResult cancel(const CancelOrder&) = 0;
    virtual CommandResult reduce(const ReduceOrder&) = 0;
    virtual CommandResult move(const MoveOrder&, Seq) = 0;
    virtual std::optional<RestingOrder> find_order(OrderId) const = 0;
    virtual std::vector<RestingOrder> resting_orders() const = 0;
    virtual L2Snapshot snapshot(std::size_t depth) const = 0;
    virtual bool validate() const = 0;
};

std::unique_ptr<Book> make_book(BookKind kind, SymbolId symbol,
                                std::size_t capacity, SymbolType type = SymbolType::spot);

class OrderBook final : public Book {
public:
    explicit OrderBook(SymbolId symbol, BookKind kind = BookKind::list,
                       std::size_t capacity = 4096, SymbolType type = SymbolType::spot);
    std::unique_ptr<Book> clone() const override;
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<RestingOrder> find_order(OrderId id) const override;
    bool validate() const override;

    L2Snapshot snapshot(std::size_t depth) const override;
    std::vector<RestingOrder> resting_orders() const override;

    CommandResult place(const PlaceOrder& order, Seq seq) override;
    CommandResult cancel(const CancelOrder& command) override;
    CommandResult reduce(const ReduceOrder& command) override;
    CommandResult move(const MoveOrder& command, Seq seq) override;

    static std::unique_ptr<OrderBook> from_resting(
        SymbolId symbol, const std::vector<RestingOrder>& orders,
        BookKind kind = BookKind::list, std::size_t capacity = 4096,
        SymbolType type = SymbolType::spot);

private:
    friend struct OrderBookTestAccess;

    struct Level {
        explicit Level(OrderNodePool* pool) : fifo(pool) {}
        OrderQueue fifo;
        std::int64_t total_remaining{};
    };

    struct OrderLocation {
        Side side;
        Price price;
        OrderQueue::Handle node;
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
    SymbolType type_;
    BookKind kind_;
    std::size_t capacity_;
    std::unique_ptr<OrderNodePool> pool_;
    AskLevels asks_;
    BidLevels bids_;
    Index index_;
};

}  // namespace exchange

#endif
