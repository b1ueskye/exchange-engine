#include "exchange/order_book.hpp"

#include <algorithm>
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

template <class Levels>
Status OrderBook::append_to(
    Levels& levels,
    const RestingOrder& order,
    BeforeIndexInsert before_index) {
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
        try {
            if (before_index) {
                before_index();
            }
            const auto node = std::prev(level.fifo.end());
            const auto inserted = index_.try_emplace(
                order.key.order, OrderLocation{order.side, order.price, node});
            if (!inserted.second) {
                throw std::logic_error{
                    "duplicate order during index insertion"};
            }
        } catch (...) {
            level.fifo.pop_back();
            throw;
        }
    } catch (...) {
        if (created) {
            levels.erase(it);
        }
        throw;
    }

    level.total_remaining = *total;
    return Status::processed;
}

Status OrderBook::add_resting(
    const RestingOrder& order,
    BeforeIndexInsert before_index) {
    if (!valid_resting(order)) {
        return Status::invalid_input;
    }
    if (index_.contains(order.key.order)) {
        return Status::duplicate_order;
    }
    if (crosses(order)) {
        return Status::invalid_input;
    }
    return order.side == Side::ask ? append_to(asks_, order, before_index)
                                   : append_to(bids_, order, before_index);
}

std::optional<RestingOrder> OrderBook::find_order(OrderId id) const {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return std::nullopt;
    }
    return *it->second.node;
}

std::optional<RestingOrder>
OrderBook::remove_resting(OrderId id, UserId owner) {
    const auto it = index_.find(id);
    if (it == index_.end() || it->second.node->user != owner) {
        return std::nullopt;
    }
    return it->second.side == Side::ask ? remove_from(asks_, it)
                                        : remove_from(bids_, it);
}

template <class Levels>
RestingOrder OrderBook::remove_from(Levels& levels, Index::iterator indexed) {
    const auto location = indexed->second;
    const auto level_it = levels.find(location.price);
    if (level_it == levels.end()) {
        throw std::logic_error{"order index points to a missing price level"};
    }

    auto& level = level_it->second;
    const RestingOrder removed = *location.node;
    index_.erase(indexed);
    level.fifo.erase(location.node);
    level.total_remaining -= removed.remaining.value();
    if (level.fifo.empty()) {
        levels.erase(level_it);
    }
    return removed;
}

bool OrderBook::validate() const {
    std::size_t count = 0;
    const auto validate_side = [this, &count](const auto& levels, Side side) {
        for (const auto& [price, level] : levels) {
            if (level.fifo.empty() || level.total_remaining <= 0) {
                return false;
            }
            std::int64_t total = 0;
            Seq previous{0};
            for (const auto& order : level.fifo) {
                if (!valid_resting(order) || order.side != side ||
                    order.price != price || order.priority_seq <= previous) {
                    return false;
                }
                const auto it = index_.find(order.key.order);
                if (it == index_.end() || it->second.side != side ||
                    it->second.price != price ||
                    std::addressof(*it->second.node) != std::addressof(order)) {
                    return false;
                }
                const auto next = checked_add(total, order.remaining.value());
                if (!next) {
                    return false;
                }
                total = *next;
                previous = order.priority_seq;
                ++count;
            }
            if (total != level.total_remaining) {
                return false;
            }
        }
        return true;
    };

    const auto bid = best_bid();
    const auto ask = best_ask();
    return (!bid || !ask || *bid < *ask) && validate_side(asks_, Side::ask) &&
           validate_side(bids_, Side::bid) && count == index_.size();
}

L2Snapshot OrderBook::snapshot(std::size_t depth) const {
    const auto read_side = [depth](const auto& levels) {
        const auto limit = std::min(depth, levels.size());
        std::vector<PriceLevel> result;
        result.reserve(limit);
        auto it = levels.begin();
        for (std::size_t i = 0; i < limit; ++i, ++it) {
            const auto& [price, level] = *it;
            result.push_back(
                {price, Quantity{level.total_remaining}, level.fifo.size()});
        }
        return result;
    };

    return {read_side(asks_), read_side(bids_)};
}

std::vector<RestingOrder> OrderBook::resting_orders() const {
    std::vector<RestingOrder> result;
    result.reserve(index_.size());
    const auto append = [&result](const auto& levels) {
        for (const auto& [price, level] : levels) {
            result.insert(result.end(), level.fifo.begin(), level.fifo.end());
        }
    };
    append(asks_);
    append(bids_);
    return result;
}

}