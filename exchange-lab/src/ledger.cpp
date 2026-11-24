#include "exchange/ledger.hpp"

#include <algorithm>

namespace exchange {

namespace {

template <class Map, class Key>
std::int64_t value_or_zero(const Map& map, Key key) {
    const auto it = map.find(key);
    return it == map.end() ? 0 : it->second;
}

}  // namespace

std::optional<Money> required_hold(const PlaceOrder& order,
                                   const SymbolSpec& spec) {
    if (!validate_command(order) || order.key.symbol != spec.symbol) {
        return std::nullopt;
    }
    if (order.side == Side::ask) {
        if (const auto* limit = std::get_if<LimitPrice>(&order.constraint)) {
            const auto per_lot =
                quote_notional(Quantity{1}, limit->price, spec);
            if (!per_lot || per_lot->units < spec.taker_fee_per_lot) {
                return std::nullopt;
            }
        }
        return base_amount(order.quantity, spec);
    }
    if (const auto* limit = std::get_if<LimitPrice>(&order.constraint)) {
        return bid_hold(order.quantity,
                        limit->reserve_bid_price.value_or(limit->price), spec);
    }
    return budget_bid_hold(order.quantity,
                           std::get<BudgetLimit>(order.constraint).total, spec);
}

Status Ledger::add_user(UserId user) {
    if (user.value() == 0) {
        return Status::invalid_input;
    }
    return accounts_.try_emplace(user).second ? Status::processed
                                              : Status::user_exists;
}

Status Ledger::adjust(const AdjustBalance& cmd) {
    if (!validate_command(cmd)) {
        return Status::invalid_input;
    }
    const auto found = accounts_.find(cmd.user);
    if (found == accounts_.end()) {
        return Status::unknown_user;
    }
    auto& account = found->second;
    if (account.suspended) {
        return Status::user_suspended;
    }
    if (cmd.funding_id == account.last_funding) {
        return Status::duplicate_funding;
    }
    if (cmd.funding_id < account.last_funding) {
        return Status::stale_funding;
    }

    const auto next = checked_add(
        value_or_zero(account.balances, cmd.amount.currency), cmd.amount.units);
    const auto net = checked_add(
        value_or_zero(net_deposits_, cmd.amount.currency), cmd.amount.units);
    if (!next || !net) {
        return Status::invalid_input;
    }
    if (*next < 0) {
        return Status::insufficient_funds;
    }
    if (*net < 0) {
        return Status::invalid_input;
    }

    auto [balance, created] =
        account.balances.try_emplace(cmd.amount.currency, 0);
    try {
        auto [deposit, inserted] =
            net_deposits_.try_emplace(cmd.amount.currency, 0);
        deposit->second = *net;
    } catch (...) {
        if (created) {
            account.balances.erase(balance);
        }
        throw;
    }
    balance->second = *next;
    account.last_funding = cmd.funding_id;
    return Status::processed;
}

Status Ledger::suspend(UserId user) {
    const auto it = accounts_.find(user);
    if (it == accounts_.end()) {
        return Status::unknown_user;
    }
    if (it->second.suspended) {
        return Status::user_suspended;
    }
    const auto nonzero =
        std::any_of(it->second.balances.begin(), it->second.balances.end(),
                    [](const auto& row) { return row.second != 0; });
    const auto reserved = std::any_of(
        holds_.begin(), holds_.end(),
        [user](const auto& row) { return row.second.user == user; });
    if (nonzero || reserved) {
        return Status::account_not_empty;
    }
    it->second.suspended = true;
    return Status::processed;
}

Status Ledger::resume(UserId user) {
    const auto it = accounts_.find(user);
    if (it == accounts_.end()) {
        return Status::unknown_user;
    }
    if (!it->second.suspended) {
        return Status::invalid_input;
    }
    it->second.suspended = false;
    return Status::processed;
}

Status Ledger::freeze(const PlaceOrder& order, const SymbolSpec& spec) {
    const auto account = accounts_.find(order.user);
    if (account == accounts_.end()) {
        return Status::unknown_user;
    }
    if (account->second.suspended) {
        return Status::user_suspended;
    }
    const HoldKey key{order.key.symbol, order.key.order};
    if (holds_.contains(key)) {
        return Status::duplicate_order;
    }
    const auto money = required_hold(order, spec);
    if (!money || money->units <= 0) {
        return Status::invalid_input;
    }
    const auto balance = account->second.balances.find(money->currency);
    if (balance == account->second.balances.end() ||
        balance->second < money->units) {
        return Status::insufficient_funds;
    }
    holds_.try_emplace(key, Hold{order.user, *money});
    balance->second -= money->units;
    return Status::processed;
}

std::optional<Money> Ledger::available(UserId user, CurrencyId currency) const {
    const auto it = accounts_.find(user);
    if (it == accounts_.end()) {
        return std::nullopt;
    }
    return Money{currency, value_or_zero(it->second.balances, currency)};
}

std::optional<Money> Ledger::held(OrderKey key) const {
    const auto it = holds_.find({key.symbol, key.order});
    if (it == holds_.end()) {
        return std::nullopt;
    }
    return it->second.money;
}

void Ledger::swap(Ledger& other) noexcept {
    accounts_.swap(other.accounts_);
    holds_.swap(other.holds_);
    net_deposits_.swap(other.net_deposits_);
}

}  // namespace exchange