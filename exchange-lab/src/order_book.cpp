#include "exchange/order_book.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace exchange {

OrderBook::OrderBook(SymbolId symbol, BookKind kind, std::size_t capacity)
    : symbol_{symbol}, kind_{kind}, capacity_{capacity} {
    if (symbol.value() == 0) {
        throw std::invalid_argument{"order book requires a symbol"};
    }
    if (kind_ == BookKind::pooled) {
        if (capacity_ == std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument{"pool capacity too large"};
        }
        pool_ = std::make_unique<OrderNodePool>(capacity_ + 1);
    }
}

std::unique_ptr<Book> OrderBook::clone() const {
    return clone_order_book();
}

std::unique_ptr<OrderBook> OrderBook::clone_order_book() const {
    auto copy = std::make_unique<OrderBook>(symbol_, kind_, capacity_);
    for (const auto& order : resting_orders()) {
        if (copy->add_resting(order) != Status::processed) {
            throw std::logic_error{"could not clone order book"};
        }
    }
    if (!copy->validate()) {
        throw std::logic_error{"cloned order book is inconsistent"};
    }
    return copy;
}

std::unique_ptr<Book> make_book(BookKind kind, SymbolId symbol,
                                std::size_t capacity) {
    return std::make_unique<OrderBook>(symbol, kind, capacity);
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
Status OrderBook::append_to(Levels& levels, const RestingOrder& order,
                            BeforeIndexInsert before_index) {
    if (pool_ && index_.size() >= capacity_) {
        return Status::capacity_exhausted;
    }
    auto [it, created] = levels.try_emplace(order.price, pool_.get());
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
        if (!level.fifo.push_back(order)) {
            if (created) {
                levels.erase(it);
            }
            return Status::capacity_exhausted;
        }
        try {
            if (before_index) {
                before_index();
            }
            const auto node = level.fifo.last();
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

Status OrderBook::add_resting(const RestingOrder& order,
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

std::optional<RestingOrder> OrderBook::remove_resting(OrderId id,
                                                      UserId owner) {
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
            for (const auto& order : level.fifo) {
                result.push_back(order);
            }
        }
    };
    append(asks_);
    append(bids_);
    return result;
}

template <class Levels>
OrderBook::MatchPlan OrderBook::plan_against(const Levels& levels,
                                             const PlaceOrder& order,
                                             std::optional<Price> limit) const {
    auto remaining = order.quantity.value();
    std::vector<Event> events;
    for (const auto& [price, level] : levels) {
        if (limit &&
            (order.side == Side::bid ? price > *limit : price < *limit)) {
            break;
        }
        for (const auto& maker : level.fifo) {
            const auto amount = std::min(remaining, maker.remaining.value());
            remaining -= amount;
            events.emplace_back(
                Trade{maker.key, maker.user, order.key, order.user, order.side,
                      price, Quantity{amount},
                      amount == maker.remaining.value(), remaining == 0});
            if (remaining == 0) {
                return {Quantity{0}, std::move(events)};
            }
        }
    }
    return {Quantity{remaining}, std::move(events)};
}

OrderBook::MatchPlan OrderBook::plan_match(const PlaceOrder& order,
                                           std::optional<Price> limit) const {
    return order.side == Side::bid ? plan_against(asks_, order, limit)
                                   : plan_against(bids_, order, limit);
}

template <class Levels>
void OrderBook::decrease_in(Levels& levels, Index::iterator indexed,
                            Quantity amount) {
    const auto location = indexed->second;
    auto& maker = *location.node;
    if (amount == maker.remaining) {
        remove_from(levels, indexed);
        return;
    }
    auto& level = levels.at(location.price);
    maker.remaining = Quantity{maker.remaining.value() - amount.value()};
    level.total_remaining -= amount.value();
}

void OrderBook::decrease_resting(Index::iterator indexed, Quantity amount) {
    if (indexed == index_.end() || amount.value() <= 0 ||
        amount > indexed->second.node->remaining) {
        throw std::logic_error{"invalid resting order reduction"};
    }
    if (indexed->second.side == Side::ask) {
        decrease_in(asks_, indexed, amount);
    } else {
        decrease_in(bids_, indexed, amount);
    }
}

template <class Levels>
Status OrderBook::check_level(const Levels& levels, const RestingOrder& order,
                              const RestingOrder* replaced) const {
    const auto target = levels.find(order.price);
    if (target != levels.end()) {
        const auto& level = target->second;
        const auto replaced_quantity =
            replaced && replaced->price == order.price
                ? replaced->remaining.value()
                : 0;
        if (level.fifo.back().priority_seq >= order.priority_seq ||
            !checked_add(level.total_remaining - replaced_quantity,
                         order.remaining.value())) {
            return Status::invalid_input;
        }
    }
    return pool_ && !pool_->has_room() ? Status::capacity_exhausted
                                       : Status::processed;
}

Status OrderBook::check_remainder(const RestingOrder& order,
                                  const RestingOrder* replaced) const {
    if (!replaced && pool_ && index_.size() >= capacity_) {
        return Status::capacity_exhausted;
    }
    return order.side == Side::ask ? check_level(asks_, order, replaced)
                                   : check_level(bids_, order, replaced);
}

OrderBook::Prepared OrderBook::prepare(const PlaceOrder& order, Seq seq) const {
    if (!validate_command(order) || seq.value() == 0) {
        return {{Status::invalid_input, {}}};
    }
    if (order.key.symbol != symbol_) {
        return {{Status::unknown_symbol, {}}};
    }
    if (index_.contains(order.key.order)) {
        return {{Status::duplicate_order, {}}};
    }

    if (const auto* budget = std::get_if<BudgetLimit>(&order.constraint)) {
        return prepare_budget(order, *budget);
    }
    const auto& limit = std::get<LimitPrice>(order.constraint);
    auto match = plan_match(order, limit.price);
    std::optional<RestingOrder> after;
    if (match.remaining.value() > 0 && limit.policy == LimitPolicy::ioc) {
        match.events.emplace_back(RejectRemainder{order.key, order.user,
                                                  match.remaining,
                                                  RejectReason::ioc_remainder});
    } else if (match.remaining.value() > 0) {
        std::optional<Price> reserve;
        if (order.side == Side::bid) {
            reserve = limit.reserve_bid_price.value_or(limit.price);
        }
        after.emplace(RestingOrder{order.key, order.user, order.side,
                                   limit.price, match.remaining, reserve, seq,
                                   order.timestamp_ns});
        const auto status = check_remainder(*after);
        if (status != Status::processed) {
            return {{status, {}}};
        }
    }
    return {
        {Status::processed, std::move(match.events)}, order.key.order, after};
}

template <class Levels>
OrderBook::BudgetTrial OrderBook::trial_budget(const Levels& levels,
                                               Quantity quantity) const {
    auto remaining = quantity.value();
    std::int64_t total = 0;
    for (const auto& [price, level] : levels) {
        const auto amount = std::min(remaining, level.total_remaining);
        const auto line = checked_multiply(amount, price.value());
        if (!line) {
            return RejectReason::arithmetic_limit;
        }
        const auto next = checked_add(total, *line);
        if (!next) {
            return RejectReason::arithmetic_limit;
        }
        total = *next;
        remaining -= amount;
        if (remaining == 0) {
            return NotionalSteps{total};
        }
    }
    return RejectReason::insufficient_liquidity;
}

OrderBook::Prepared OrderBook::prepare_budget(const PlaceOrder& order,
                                              BudgetLimit budget) const {
    const auto reject = [&order](RejectReason reason) -> Prepared {
        return {
            {Status::processed,
             {RejectRemainder{order.key, order.user, order.quantity, reason}}},
            order.key.order};
    };
    const auto trial = order.side == Side::bid
                           ? trial_budget(asks_, order.quantity)
                           : trial_budget(bids_, order.quantity);
    if (const auto* failure = std::get_if<RejectReason>(&trial)) {
        return reject(*failure);
    }
    const auto total = std::get<NotionalSteps>(trial);
    if (order.side == Side::bid ? total > budget.total : total < budget.total) {
        return reject(RejectReason::budget_limit);
    }

    auto plan = plan_match(order, std::nullopt);
    if (plan.remaining.value() != 0) {
        throw std::logic_error{"budget trial disagrees with price levels"};
    }
    return {{Status::processed, std::move(plan.events)}, order.key.order};
}

OrderBook::Prepared OrderBook::prepare_reduce(
    OrderKey key, UserId user, std::optional<Quantity> requested) const {
    if (key.symbol != symbol_) {
        return {{Status::unknown_symbol, {}}};
    }
    const auto indexed = index_.find(key.order);
    if (indexed == index_.end() || indexed->second.node->user != user) {
        return {{Status::unknown_order, {}}};
    }
    const auto& order = *indexed->second.node;
    const auto amount =
        std::min(requested.value_or(order.remaining), order.remaining);
    const bool removed = amount == order.remaining;
    std::optional<RestingOrder> after;
    if (!removed) {
        after = order;
        after->remaining = Quantity{order.remaining.value() - amount.value()};
    }
    return {{Status::processed,
             {Reduce{key, user, order.side, order.price, amount, removed}}},
            key.order,
            after,
            amount};
}

OrderBook::Prepared OrderBook::prepare(const CancelOrder& command, Seq) const {
    if (!validate_command(command)) {
        return {{Status::invalid_input, {}}};
    }
    return prepare_reduce(command.key, command.user, std::nullopt);
}

OrderBook::Prepared OrderBook::prepare(const ReduceOrder& command, Seq) const {
    if (!validate_command(command)) {
        return {{Status::invalid_input, {}}};
    }
    return prepare_reduce(command.key, command.user, command.quantity);
}

template <class Levels>
Status OrderBook::replace_resting(Levels& levels, Index::iterator indexed,
                                  const RestingOrder& order) {
    const auto old_location = indexed->second;
    const auto old_level = levels.find(old_location.price);
    const auto old_quantity = old_location.node->remaining.value();
    auto [target, created] = levels.try_emplace(order.price, pool_.get());
    auto& level = target->second;
    const bool same_level = target == old_level;
    const auto base = level.total_remaining - (same_level ? old_quantity : 0);
    const auto total = checked_add(base, order.remaining.value());
    if ((!level.fifo.empty() &&
         level.fifo.back().priority_seq >= order.priority_seq) ||
        !total) {
        return Status::invalid_input;
    }

    try {
        if (!level.fifo.push_back(order)) {
            if (created) {
                levels.erase(target);
            }
            return Status::capacity_exhausted;
        }
    } catch (...) {
        if (created) {
            levels.erase(target);
        }
        throw;
    }

    indexed->second = OrderLocation{order.side, order.price, level.fifo.last()};
    old_level->second.fifo.erase(old_location.node);
    level.total_remaining = *total;
    if (!same_level) {
        old_level->second.total_remaining -= old_quantity;
        if (old_level->second.fifo.empty()) {
            levels.erase(old_level);
        }
    }
    return Status::processed;
}

OrderBook::Prepared OrderBook::prepare(const MoveOrder& command,
                                       Seq seq) const {
    if (!validate_command(command) || seq.value() == 0) {
        return {{Status::invalid_input, {}}};
    }
    if (command.key.symbol != symbol_) {
        return {{Status::unknown_symbol, {}}};
    }
    const auto indexed = index_.find(command.key.order);
    if (indexed == index_.end() || indexed->second.node->user != command.user) {
        return {{Status::unknown_order, {}}};
    }
    const auto& original = *indexed->second.node;
    auto moved = original;
    if (seq <= moved.priority_seq) {
        return {{Status::invalid_input, {}}};
    }
    if (moved.reserve_bid_price &&
        *moved.reserve_bid_price < command.new_price) {
        return {{Status::price_over_reserve, {}}};
    }
    moved.price = command.new_price;
    moved.priority_seq = seq;
    const PlaceOrder taker{
        moved.key,
        moved.user,
        moved.side,
        moved.remaining,
        LimitPrice{moved.price, LimitPolicy::gtc, moved.reserve_bid_price},
        moved.timestamp_ns};
    auto match = plan_match(taker, moved.price);
    if (match.remaining.value() == 0) {
        return {{Status::processed, std::move(match.events)},
                command.key.order,
                {},
                original.remaining};
    }

    moved.remaining = match.remaining;
    const auto status = check_remainder(moved, &original);
    if (status != Status::processed) {
        return {{status, {}}};
    }
    return {{Status::processed, std::move(match.events)},
            command.key.order,
            moved,
            Quantity{0},
            true};
}

CommandResult OrderBook::apply(Prepared plan) {
    if (plan.outcome.status != Status::processed) {
        return std::move(plan.outcome);
    }
    if (plan.reduction.value() != 0) {
        decrease_resting(index_.find(plan.primary), plan.reduction);
    } else if (plan.after) {
        const auto& order = *plan.after;
        const auto update = [&](auto& levels) {
            return plan.replace ? replace_resting(
                                      levels, index_.find(plan.primary), order)
                                : append_to(levels, order, nullptr);
        };
        const auto status =
            order.side == Side::ask ? update(asks_) : update(bids_);
        if (status != Status::processed) {
            return {status, {}};
        }
    }
    for (const auto& event : plan.outcome.events) {
        if (const auto* trade = std::get_if<Trade>(&event)) {
            decrease_resting(index_.find(trade->maker.order), trade->quantity);
        }
    }
    return std::move(plan.outcome);
}

CommandResult OrderBook::place(const PlaceOrder& order, Seq seq) {
    return apply(prepare(order, seq));
}

CommandResult OrderBook::cancel(const CancelOrder& command) {
    return apply(prepare(command, Seq{0}));
}

CommandResult OrderBook::reduce(const ReduceOrder& command) {
    return apply(prepare(command, Seq{0}));
}

CommandResult OrderBook::move(const MoveOrder& command, Seq seq) {
    return apply(prepare(command, seq));
}

std::unique_ptr<OrderBook> OrderBook::from_resting(
    SymbolId symbol, const std::vector<RestingOrder>& orders, BookKind kind,
    std::size_t capacity) {
    auto staged = std::make_unique<OrderBook>(symbol, kind, capacity);
    for (const auto& order : orders) {
        if (staged->add_resting(order) != Status::processed) {
            throw std::invalid_argument{"invalid resting state"};
        }
    }
    if (!staged->validate()) {
        throw std::logic_error{"rebuilt order book is inconsistent"};
    }
    return staged;
}
}  // namespace exchange
