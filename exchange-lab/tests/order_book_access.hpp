#ifndef EXCHANGE_ORDER_BOOK_ACCESS_HPP
#define EXCHANGE_ORDER_BOOK_ACCESS_HPP

#include "exchange/order_book.hpp"

#include <vector>

namespace exchange {

struct OrderBookTestAccess {
    static Status insert(OrderBook& book, const RestingOrder& order) {
        return book.add_resting(order);
    }

    static std::vector<RestingOrder> queue(const OrderBook& book, Side side,
                                           Price price) {
        const auto read =
            [price](const auto& levels) -> std::vector<RestingOrder> {
            const auto it = levels.find(price);
            if (it == levels.end()) {
                return {};
            }
            std::vector<RestingOrder> result;
            for (const auto& order : it->second.fifo) {
                result.push_back(order);
            }
            return result;
        };
        return side == Side::ask ? read(book.asks_) : read(book.bids_);
    }

    static std::optional<std::int64_t> total(const OrderBook& book, Side side,
                                             Price price) {
        const auto read =
            [price](const auto& levels) -> std::optional<std::int64_t> {
            const auto it = levels.find(price);
            if (it == levels.end()) {
                return std::nullopt;
            }
            return it->second.total_remaining;
        };
        return side == Side::ask ? read(book.asks_) : read(book.bids_);
    }

    static std::optional<RestingOrder> remove(OrderBook& book, OrderId id,
                                              UserId owner) {
        return book.remove_resting(id, owner);
    }

    static Status inject_failure(OrderBook& book, const RestingOrder& order,
                                 void (*fail)()) {
        return book.add_resting(order, fail);
    }

    static std::size_t index_bucket_count(const OrderBook& book) {
        return book.index_.bucket_count();
    }

    static void rehash_index(OrderBook& book, std::size_t buckets) {
        book.index_.rehash(buckets);
    }
};

}  // namespace exchange

#endif