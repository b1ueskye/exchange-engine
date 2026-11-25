#include "exchange/ledger.hpp"
#include "spot_update.hpp"

#include <algorithm>
#include <stdexcept>

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

Status Ledger::check_freeze(const PlaceOrder& order, const SymbolSpec& spec,
                            Freeze& freeze) {
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
    freeze = {&balance->second, Hold{order.user, *money}};
    return Status::processed;
}

Status Ledger::freeze(const PlaceOrder& order, const SymbolSpec& spec) {
    Freeze freeze;
    const auto status = check_freeze(order, spec, freeze);
    if (status == Status::processed) {
        holds_.try_emplace({order.key.symbol, order.key.order}, freeze.hold);
        *freeze.balance -= freeze.hold.money.units;
    }
    return status;
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

Ledger::SpotUpdate::AmountChange::AmountChange(Amounts& map,
                                               CurrencyId currency,
                                               std::int64_t* known)
    : target(&map), existing(known) {
    if (!existing) {
        const auto found = map.find(currency);
        if (found != map.end()) {
            existing = &found->second;
        } else {
            Amounts storage;
            pending = storage.extract(storage.try_emplace(currency, 0).first);
        }
    }
    value = existing ? *existing : 0;
}

void Ledger::SpotUpdate::AmountChange::commit() {
    if (existing) {
        *existing = value;
    } else {
        pending.mapped() = value;
        target->insert(std::move(pending));
    }
}

Ledger::SpotUpdate::SpotUpdate(Ledger& ledger, OrderKey primary,
                               const SymbolSpec& spec)
    : ledger_(ledger),
      primary_key_(primary.symbol, primary.order),
      spec_(spec),
      primary_existing_(ledger.holds_.end()) {}

Status Ledger::SpotUpdate::freeze(const PlaceOrder& order) {
    Freeze freeze;
    const auto status = ledger_.check_freeze(order, spec_, freeze);
    if (status == Status::processed) {
        // Seed the overlay before settlement, including when the user trades
        // on both sides and receives a credit in the same currency.
        auto& balances = ledger_.accounts_.find(order.user)->second.balances;
        const auto amount = freeze.hold.money;
        balance(order.user, balances, amount.currency, freeze.balance).value -=
            amount.units;
        primary_ = freeze.hold;
    }
    return status;
}

Ledger::SpotUpdate::AmountChange& Ledger::SpotUpdate::balance(
    UserId user, Amounts& amounts, CurrencyId currency, std::int64_t* known) {
    const BalanceKey key{user, currency};
    for (std::size_t i = 0; i < inline_count_; ++i) {
        if (inline_balances_[i]->first == key) {
            return inline_balances_[i]->second;
        }
    }
    if (inline_count_ < inline_balances_.size()) {
        auto& slot = inline_balances_[inline_count_];
        slot.emplace(key, AmountChange{amounts, currency, known});
        ++inline_count_;
        return slot->second;
    }
    return extra_balances_.try_emplace(key, amounts, currency, known)
        .first->second;
}

bool Ledger::SpotUpdate::credit(UserId user, Money amount) {
    const auto account = ledger_.accounts_.find(user);
    if (account == ledger_.accounts_.end() || amount.units < 0) {
        return false;
    }
    auto& change = balance(user, account->second.balances, amount.currency);
    const auto next = checked_add(change.value, amount.units);
    if (!next) {
        return false;
    }
    change.value = *next;
    return true;
}

bool Ledger::SpotUpdate::consume(Hold& hold, UserId user, Money amount) {
    if (hold.user != user || hold.money.currency != amount.currency ||
        amount.units < 0 || amount.units > hold.money.units) {
        return false;
    }
    hold.money.units -= amount.units;
    return true;
}

bool Ledger::SpotUpdate::apply_trade(const Trade& trade, Hold& maker) {
    const auto& spec = spec_;
    const bool buy_takes = trade.taker_side == Side::bid;
    auto& buy_hold = buy_takes ? *primary_ : maker;
    auto& sell_hold = buy_takes ? maker : *primary_;
    const auto buyer = buy_takes ? trade.taker_user : trade.maker_user;
    const auto seller = buy_takes ? trade.maker_user : trade.taker_user;
    const auto buy_fee_rate =
        buy_takes ? spec.taker_fee_per_lot : spec.maker_fee_per_lot;
    const auto sell_fee_rate =
        buy_takes ? spec.maker_fee_per_lot : spec.taker_fee_per_lot;
    const auto base = base_amount(trade.quantity, spec);
    const auto quote = quote_notional(trade.quantity, trade.price, spec);
    const auto buy_fee = checked_multiply(trade.quantity.value(), buy_fee_rate);
    const auto sell_fee =
        checked_multiply(trade.quantity.value(), sell_fee_rate);
    if (!base || !quote || !buy_fee || !sell_fee || quote->units < *sell_fee) {
        return false;
    }

    const auto buy_paid = checked_add(quote->units, *buy_fee);
    const auto sell_received = checked_subtract(quote->units, *sell_fee);
    const auto fees_paid = checked_add(*buy_fee, *sell_fee);
    if (!buy_paid || !sell_received || !fees_paid) {
        return false;
    }
    if (!fee_) {
        fee_.emplace(ledger_.fees_, spec.quote, nullptr);
    }
    const auto fees_after = checked_add(fee_->value, *fees_paid);
    if (!fees_after || !consume(buy_hold, buyer, {spec.quote, *buy_paid}) ||
        !consume(sell_hold, seller, *base) || !credit(buyer, *base) ||
        !credit(seller, {spec.quote, *sell_received})) {
        return false;
    }
    fee_->value = *fees_after;
    return true;
}

bool Ledger::SpotUpdate::align_hold(Hold& hold,
                                    const std::optional<RestingOrder>& order) {
    std::int64_t keep = 0;
    if (order) {
        if (order->user != hold.user ||
            (order->side == Side::bid && !order->reserve_bid_price)) {
            return false;
        }
        const auto required =
            order->side == Side::bid
                ? bid_hold(order->remaining, *order->reserve_bid_price, spec_)
                : base_amount(order->remaining, spec_);
        if (!required || required->currency != hold.money.currency ||
            required->units > hold.money.units) {
            return false;
        }
        keep = required->units;
    }

    const auto refund = hold.money.units - keep;
    if (refund > 0 && !credit(hold.user, {hold.money.currency, refund})) {
        return false;
    }
    hold.money.units = keep;
    return true;
}

bool Ledger::SpotUpdate::prepare(
    const OrderBook& book, const std::vector<Event>& events,
    const std::optional<RestingOrder>& primary_after) {
    if (!primary_) {
        primary_existing_ = ledger_.holds_.find(primary_key_);
        if (primary_existing_ == ledger_.holds_.end()) {
            return false;
        }
        primary_ = primary_existing_->second;
    }

    for (const auto& event : events) {
        if (const auto* trade = std::get_if<Trade>(&event)) {
            const auto found =
                ledger_.holds_.find({trade->maker.symbol, trade->maker.order});
            if (found == ledger_.holds_.end()) {
                return false;
            }
            if (makers_.empty()) {
                makers_.reserve(events.size());
            }
            // A single command visits each maker at most once.
            auto& maker = makers_.emplace_back(found, found->second, trade);
            if (!apply_trade(*trade, maker.hold)) {
                return false;
            }
        }
    }

    // Preserve the reference implementation's order of refunds. Settlement
    // applies all trade debits and credits before aligning any hold.
    std::sort(makers_.begin(), makers_.end(),
              [](const auto& left, const auto& right) {
                  return left.existing->first < right.existing->first;
              });
    bool primary_aligned = false;
    for (auto& maker : makers_) {
        if (!primary_aligned && primary_key_ < maker.existing->first) {
            if (!align_hold(*primary_, primary_after)) return false;
            primary_aligned = true;
        }
        std::optional<RestingOrder> after;
        if (!maker.trade->maker_filled) {
            after = book.find_order(maker.trade->maker.order);
            if (!after || after->remaining <= maker.trade->quantity)
                return false;
            after->remaining = Quantity{after->remaining.value() -
                                        maker.trade->quantity.value()};
        }
        if (!align_hold(maker.hold, after)) return false;
    }
    if (!primary_aligned && !align_hold(*primary_, primary_after)) {
        return false;
    }
    if (primary_existing_ == ledger_.holds_.end() &&
        primary_->money.units != 0) {
        Holds storage;
        primary_pending_ =
            storage.extract(storage.try_emplace(primary_key_, *primary_).first);
    }
    return true;
}

void Ledger::SpotUpdate::commit() {
    // Every missing node has already been allocated using the same default
    // allocator. Node transfers compare only integer IDs and do not allocate.
    for (std::size_t i = 0; i < inline_count_; ++i) {
        inline_balances_[i]->second.commit();
    }
    for (auto& [key, change] : extra_balances_) {
        change.commit();
    }
    if (fee_) fee_->commit();

    const auto update = [this](Holds::iterator existing, const Hold& hold) {
        if (hold.money.units == 0) {
            ledger_.holds_.erase(existing);
        } else {
            existing->second = hold;
        }
    };
    for (const auto& maker : makers_) {
        update(maker.existing, maker.hold);
    }
    if (primary_existing_ != ledger_.holds_.end()) {
        update(primary_existing_, *primary_);
    } else if (primary_pending_) {
        ledger_.holds_.insert(std::move(primary_pending_));
    }
}

bool CurrencyTotals::conserved() const {
    const auto reserved = checked_add(available, frozen);
    if (!reserved) {
        return false;
    }
    const auto total = checked_add(*reserved, fees);
    return total && *total == net_deposits;
}

std::optional<UserReport> Ledger::user_report(UserId user) const {
    const auto it = accounts_.find(user);
    if (it == accounts_.end()) {
        return std::nullopt;
    }
    UserReport report{
        user, it->second.suspended, it->second.last_funding, {}, {}, {}};
    for (const auto& [currency, units] : it->second.balances) {
        report.available.push_back({currency, units});
    }
    for (const auto& [key, hold] : holds_) {
        if (hold.user == user) {
            report.frozen.push_back({{key.first, key.second}, hold.money});
        }
    }
    return report;
}

TotalsReport Ledger::totals() const {
    std::map<CurrencyId, CurrencyTotals> rows;
    const auto row = [&rows](CurrencyId currency) -> CurrencyTotals& {
        return rows.try_emplace(currency, CurrencyTotals{currency})
            .first->second;
    };
    const auto accumulate = [](std::int64_t& field, std::int64_t amount) {
        const auto next = checked_add(field, amount);
        if (!next) {
            throw std::logic_error{"ledger report total out of range"};
        }
        field = *next;
    };

    for (const auto& account : accounts_) {
        for (const auto& [currency, units] : account.second.balances) {
            accumulate(row(currency).available, units);
        }
    }
    for (const auto& entry : holds_) {
        const auto& amount = entry.second.money;
        accumulate(row(amount.currency).frozen, amount.units);
    }
    for (const auto& [currency, units] : fees_) {
        row(currency).fees = units;
    }
    for (const auto& [currency, units] : net_deposits_) {
        row(currency).net_deposits = units;
    }

    TotalsReport report;
    report.currencies.reserve(rows.size());
    for (const auto& entry : rows) {
        report.currencies.push_back(entry.second);
    }
    return report;
}

void Ledger::append_canonical(std::string& out) const {
    const auto number = [&out](auto value) {
        out += std::to_string(value);
        out += ',';
    };
    for (const auto& [id, account] : accounts_) {
        out += "user:";
        number(id.value());
        number(account.last_funding);
        number(account.suspended);
        for (const auto& [currency, units] : account.balances) {
            out += "available:";
            number(currency.value());
            number(units);
        }
    }
    for (const auto& [key, hold] : holds_) {
        out += "frozen:";
        number(key.first.value());
        number(key.second.value());
        number(hold.user.value());
        number(hold.money.currency.value());
        number(hold.money.units);
    }
    for (const auto& [currency, units] : fees_) {
        out += "fees:";
        number(currency.value());
        number(units);
    }
    for (const auto& [currency, units] : net_deposits_) {
        out += "net:";
        number(currency.value());
        number(units);
    }
}

}  // namespace exchange
