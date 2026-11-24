#include "exchange/ledger.hpp"
#include "exchange/order_book.hpp"

#include <algorithm>
#include <set>

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
    fees_.swap(other.fees_);
}

Money Ledger::fees(CurrencyId currency) const {
    return {currency, value_or_zero(fees_, currency)};
}

bool Ledger::consume(OrderKey key, UserId user, Money amount) {
    const auto it = holds_.find({key.symbol, key.order});
    if (it == holds_.end() || it->second.user != user ||
        it->second.money.currency != amount.currency ||
        amount.units < 0 || amount.units > it->second.money.units) {
        return false;
    }
    it->second.money.units -= amount.units;
    return true;
}

bool Ledger::credit(UserId user, Money amount) {
    const auto it = accounts_.find(user);
    if (it == accounts_.end() || amount.units < 0) {
        return false;
    }
    auto& balances = it->second.balances;
    const auto next = checked_add(value_or_zero(balances, amount.currency), amount.units);
    if (!next) {
        return false;
    }
    balances.try_emplace(amount.currency, 0).first->second = *next;
    return true;
}

bool Ledger::apply_trade(const Trade& trade, const SymbolSpec& spec) {
    const bool buy_takes = trade.taker_side == Side::bid;
    const auto buy_key = buy_takes ? trade.taker : trade.maker;
    const auto sell_key = buy_takes ? trade.maker : trade.taker;
    const auto buyer = buy_takes ? trade.taker_user : trade.maker_user;
    const auto seller = buy_takes ? trade.maker_user : trade.taker_user;
    const auto buy_fee_rate = buy_takes ? spec.taker_fee_per_lot : spec.maker_fee_per_lot;
    const auto sell_fee_rate = buy_takes ? spec.maker_fee_per_lot : spec.taker_fee_per_lot;
    const auto base = base_amount(trade.quantity, spec);
    const auto quote = quote_notional(trade.quantity, trade.price, spec);
    const auto buy_fee = checked_multiply(trade.quantity.value(), buy_fee_rate);
    const auto sell_fee = checked_multiply(trade.quantity.value(), sell_fee_rate);
    if (!base || !quote || !buy_fee || !sell_fee || quote->units < *sell_fee) {
        return false;
    }

    const auto buy_paid = checked_add(quote->units, *buy_fee);
    const auto sell_received = checked_subtract(quote->units, *sell_fee);
    const auto fees_paid = checked_add(*buy_fee, *sell_fee);
    if (!buy_paid || !sell_received || !fees_paid) {
        return false;
    }
    const auto fees_after = checked_add(value_or_zero(fees_, spec.quote), *fees_paid);
    if (!fees_after || !consume(buy_key, buyer, {spec.quote, *buy_paid}) ||
        !consume(sell_key, seller, *base) || !credit(buyer, *base) ||
        !credit(seller, {spec.quote, *sell_received})) {
        return false;
    }
    fees_.try_emplace(spec.quote, 0).first->second = *fees_after;
    return true;
}

bool Ledger::align_hold(OrderKey key, const OrderBook& book, const SymbolSpec& spec) {
    const auto hold = holds_.find({key.symbol, key.order});
    if (hold == holds_.end()) {
        return false;
    }

    std::int64_t keep = 0;
    const auto order = book.find_order(key.order);
    if (order) {
        if (order->user != hold->second.user ||
            (order->side == Side::bid && !order->reserve_bid_price)) {
            return false;
        }
        const auto required = order->side == Side::bid
            ? bid_hold(order->remaining, *order->reserve_bid_price, spec)
            : base_amount(order->remaining, spec);
        if (!required || required->currency != hold->second.money.currency ||
            required->units > hold->second.money.units) {
            return false;
        }
        keep = required->units;
    }

    const auto refund = hold->second.money.units - keep;
    if (refund > 0 && !credit(hold->second.user, {hold->second.money.currency, refund})) {
        return false;
    }
    if (keep == 0) {
        holds_.erase(hold);
    } else {
        hold->second.money.units = keep;
    }
    return true;
}

bool Ledger::apply_spot(OrderKey primary, const std::vector<Event>& events,
                        const OrderBook& book, const SymbolSpec& spec) {
    std::set<HoldKey> touched{{primary.symbol, primary.order}};
    for (const auto& event : events) {
        if (const auto* trade = std::get_if<Trade>(&event)) {
            if (!apply_trade(*trade, spec)) {
                return false;
            }
            touched.emplace(trade->maker.symbol, trade->maker.order);
        }
    }
    for (const auto& [symbol, id] : touched) {
        if (!align_hold({symbol, id}, book, spec)) {
            return false;
        }
    }
    return true;
}

}  // namespace exchange
