#ifndef EXCHANGE_REFERENCE_BOOK_HPP
#define EXCHANGE_REFERENCE_BOOK_HPP

#include "exchange/order_book.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace matching_test {

using namespace exchange;

class ReferenceBook {
public:
    CommandResult execute(const Command& command, Seq seq) {
        if (const auto* place = std::get_if<PlaceOrder>(&command)) {
            if (place->quantity.value() <= 0) {
                return {Status::invalid_input, {}};
            }
            if (locate(place->key) != orders_.size()) {
                return {Status::duplicate_order, {}};
            }
            return submit(*place, seq);
        }
        if (const auto* cancel = std::get_if<CancelOrder>(&command)) {
            return reduce(cancel->key, cancel->user, std::nullopt);
        }
        if (const auto* request = std::get_if<ReduceOrder>(&command)) {
            if (request->quantity.value() <= 0) {
                return {Status::invalid_input, {}};
            }
            return reduce(request->key, request->user, request->quantity);
        }
        const auto& move = std::get<MoveOrder>(command);
        const auto pos = locate(move.key);
        if (pos == orders_.size() || orders_[pos].user != move.user) {
            return {Status::unknown_order, {}};
        }
        const auto old = orders_[pos];
        if (old.reserve_bid_price && move.new_price > *old.reserve_bid_price) {
            return {Status::price_over_reserve, {}};
        }
        erase(pos);
        return submit(PlaceOrder{old.key, old.user, old.side, old.remaining,
                                LimitPrice{move.new_price, LimitPolicy::gtc, old.reserve_bid_price},
                                old.timestamp_ns}, seq);
    }

    std::vector<RestingOrder> canonical() const {
        auto copy = orders_;
        std::sort(copy.begin(), copy.end(), [](const RestingOrder& a, const RestingOrder& b) {
            if (a.side != b.side) {
                return a.side == Side::ask;
            }
            return before(a, b);
        });
        return copy;
    }

    L2Snapshot snapshot(std::size_t depth) const {
        L2Snapshot data;
        for (const auto& order : canonical()) {
            auto& side = order.side == Side::ask ? data.asks : data.bids;
            if (!side.empty() && side.back().price == order.price) {
                auto& level = side.back();
                level.total_remaining = Quantity{
                    level.total_remaining.value() + order.remaining.value()};
                ++level.order_count;
            } else if (side.size() < depth) {
                side.push_back({order.price, order.remaining, 1});
            }
        }
        return data;
    }

private:
    static bool before(const RestingOrder& a, const RestingOrder& b) {
        if (a.price == b.price) {
            return a.priority_seq < b.priority_seq;
        }
        return a.side == Side::ask ? a.price < b.price : a.price > b.price;
    }

    std::size_t locate(OrderKey key) const {
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            if (orders_[i].key == key) {
                return i;
            }
        }
        return orders_.size();
    }

    void erase(std::size_t pos) {
        orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    CommandResult reduce(OrderKey key, UserId user, std::optional<Quantity> requested) {
        const auto pos = locate(key);
        if (pos == orders_.size() || orders_[pos].user != user) {
            return {Status::unknown_order, {}};
        }
        const auto old = orders_[pos];
        const auto cut = std::min(requested.value_or(old.remaining), old.remaining);
        const bool removed = cut == old.remaining;
        if (removed) {
            erase(pos);
        } else {
            orders_[pos].remaining = Quantity{old.remaining.value() - cut.value()};
        }
        return {Status::processed, {Reduce{key, user, old.side, old.price, cut, removed}}};
    }

    std::size_t best_for(const std::vector<RestingOrder>& rows, Side taker) const {
        auto best = rows.size();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].side != taker && (best == rows.size() || before(rows[i], rows[best]))) {
                best = i;
            }
        }
        return best;
    }

    CommandResult submit(const PlaceOrder& order, Seq seq) {
        auto next = orders_;
        auto quantity = order.quantity.value();
        std::int64_t total = 0;
        CommandResult result{Status::processed, {}};
        const auto* limit = std::get_if<LimitPrice>(&order.constraint);
        while (quantity > 0) {
            const auto pos = best_for(next, order.side);
            if (pos == next.size()) {
                break;
            }
            const auto maker = next[pos];
            if (limit && (order.side == Side::bid ? maker.price > limit->price
                                                 : maker.price < limit->price)) {
                break;
            }
            const auto size = std::min(quantity, maker.remaining.value());
            quantity -= size;
            total += size * maker.price.value();
            const bool maker_done = size == maker.remaining.value();
            result.events.emplace_back(Trade{maker.key, maker.user, order.key, order.user,
                                             order.side, maker.price, Quantity{size},
                                             maker_done, quantity == 0});
            if (maker_done) {
                next.erase(next.begin() + static_cast<std::ptrdiff_t>(pos));
            } else {
                next[pos].remaining = Quantity{maker.remaining.value() - size};
            }
        }

        if (const auto* budget = std::get_if<BudgetLimit>(&order.constraint)) {
            const bool outside = order.side == Side::bid ? total > budget->total.value()
                                                        : total < budget->total.value();
            if (quantity > 0 || outside) {
                return {Status::processed, {RejectRemainder{
                    order.key, order.user, order.quantity,
                    quantity > 0 ? RejectReason::insufficient_liquidity : RejectReason::budget_limit}}};
            }
        } else if (quantity > 0 && limit->policy == LimitPolicy::gtc) {
            std::optional<Price> reserve;
            if (order.side == Side::bid) {
                reserve = limit->reserve_bid_price.value_or(limit->price);
            }
            next.push_back({order.key, order.user, order.side, limit->price, Quantity{quantity},
                            reserve, seq, order.timestamp_ns});
        } else if (quantity > 0) {
            result.events.emplace_back(RejectRemainder{
                order.key, order.user, Quantity{quantity}, RejectReason::ioc_remainder});
        }

        orders_.swap(next);
        return result;
    }

    std::vector<RestingOrder> orders_;
};

}

#endif