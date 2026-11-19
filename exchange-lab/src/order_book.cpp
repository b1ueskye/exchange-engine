#include "exchange/order_book.hpp"

#include <stdexcept>

namespace exchange {

OrderBook::OrderBook(SymbolId symbol) : symbol_{symbol} {
    if (symbol.value() == 0) {
        throw std::invalid_argument("SymbolId must be non-zero");
    }
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

bool OrderBook::valid_resting(const RestingOrder& order) const {
    if (order.key.symbol != symbol_ || order.key.order.value() == 0 ||
        order.user.value() == 0 || order.price.value() <= 0 ||
        order.remaining.value() <= 0 || order.priority_seq.value() == 0 ||
        order.timestamp_ns < 0) {
        return false;
    }
    if (order.side == Side::bid) {
        return order.reserve_bid_price &&
               order.reserve_bid_price.value() >= order.price;
    }
    return order.side == Side::ask && !order.reserve_bid_price;
}

bool OrderBook::crosses(const RestingOrder& order) const {
    if (order.side == Side::bid) {
        const auto ask = best_ask();
        return ask && order.price >= ask.value();
    }
    const auto bid = best_bid();
    return bid && order.price <= bid.value();
}

bool OrderBook::contains_order(OrderId id) const {
    const auto contains = [id](const auto& levels) {
        for (const auto& [price, level] : levels) {
            for (const auto& order : level.fifo) {
                if (order.key.order == id) {
                    return true;
                }
            }
        }
        return false;
    };

    return contains(bids_) || contains(asks_);
}

template <class Levels>
Status OrderBook::append_to(Levels& levels, const RestingOrder& order) {
    auto [it, created] = levels.try_emplace(order.price);
    auto& level = it->second;
    if (!level.fifo.empty() &&
        level.fifo.back().priority_seq >= order.priority_seq) {
        return Status::invalid_input;
    }

    const auto total =
        checked_add(level.total_remaining, order.remaining.value());
    if (!total) {
        return Status::invalid_input;
    }

    try {
        level.fifo.push_back(order);
    } catch (...) {
        if (created) {
            levels.erase(it);
        }
        throw;
    }
    level.total_remaining = total.value();
    return Status::processed;
}

Status OrderBook::add_resting(const RestingOrder& order) {
    if (!valid_resting(order)) {
        return Status::invalid_input;
    }
    if (contains_order(order.key.order)) {
        return Status::duplicate_order;
    }
    if (crosses(order)) {
        return Status::invalid_input;
    }
    if (order.side == Side::bid) {
        return append_to(bids_, order);
    }
    return append_to(asks_, order);
}

}