#include "exchange/futures_risk.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace exchange {
namespace {
std::int64_t need(std::optional<std::int64_t> v) {
    if (!v) {
        throw std::overflow_error{"margin arithmetic"};
    }
    return *v;
}
std::int64_t add(std::int64_t a, std::int64_t b) {
    return need(checked_add(a, b));
}
std::int64_t sub(std::int64_t a, std::int64_t b) {
    return need(checked_subtract(a, b));
}
std::int64_t mul(std::int64_t a, std::int64_t b) {
    return need(checked_multiply(a, b));
}
std::int64_t magnitude(std::int64_t n) {
    return n < 0 ? sub(0, n) : n;
}
template <class Map, class Key>
std::int64_t value(const Map& map, Key key) {
    const auto it = map.find(key);
    return it == map.end() ? 0 : it->second;
}
const LastPrice* price(const std::map<SymbolId, LastPrice>& prices,
                       SymbolId symbol) {
    const auto it = prices.find(symbol);
    return it == prices.end() ? nullptr : &it->second;
}
}  // namespace

std::int64_t FuturesRisk::margin(const MarginPosition& p, const SymbolSpec& s) {
    return std::max(mul(add(p.pending_buy, p.signed_quantity), s.margin_buy),
                    mul(sub(p.pending_sell, p.signed_quantity), s.margin_sell));
}

std::int64_t FuturesRisk::estimate(const MarginPosition& p, const SymbolSpec& s,
                                   const LastPrice* last) {
    if (p.signed_quantity == 0) {
        return p.profit;
    }
    if (p.signed_quantity > 0) {
        return add(p.profit,
                   last && last->bid != 0
                       ? sub(mul(p.signed_quantity, last->bid), p.cost_sum)
                       : mul(s.margin_buy, p.signed_quantity));
    }
    const auto size = magnitude(p.signed_quantity);
    return add(p.profit,
               last && last->ask != std::numeric_limits<std::int64_t>::max()
                   ? sub(p.cost_sum, mul(size, last->ask))
                   : mul(s.margin_sell, size));
}

std::int64_t FuturesRisk::update(MarginPosition& p, Side side, Quantity qty,
                                 Price price) {
    auto& pending = side == Side::bid ? p.pending_buy : p.pending_sell;
    pending = sub(pending, qty.value());
    if (pending < 0) {
        throw std::logic_error{"negative pending"};
    }
    const std::int64_t sign = side == Side::bid ? 1 : -1;
    auto opened = qty.value();
    if (p.signed_quantity && (p.signed_quantity > 0 ? 1 : -1) != sign) {
        const auto old = magnitude(p.signed_quantity);
        const auto closed = std::min(old, opened);
        const auto proceeds = mul(closed, price.value());
        if (closed < old) {
            p.cost_sum = sub(p.cost_sum, proceeds);
        } else {
            const auto difference = sub(proceeds, p.cost_sum);
            p.profit =
                add(p.profit,
                    p.signed_quantity > 0 ? difference : sub(0, difference));
            p.cost_sum = 0;
        }
        p.signed_quantity = add(p.signed_quantity, mul(sign, closed));
        opened = sub(opened, closed);
    }
    p.signed_quantity = add(p.signed_quantity, mul(sign, opened));
    p.cost_sum = add(p.cost_sum, mul(opened, price.value()));
    return opened;
}

Status FuturesRisk::reserve(Ledger& ledger, const PlaceOrder& order,
                            const SymbolSpec& spec, const SpecLookup& lookup) {
    const auto found = ledger.accounts_.find(order.user);
    if (found == ledger.accounts_.end()) {
        return Status::unknown_user;
    }
    auto& account = found->second;
    if (account.suspended) {
        return Status::user_suspended;
    }
    auto [at, created] = account.positions.try_emplace(spec.symbol);
    if (created) {
        account.position_order.insert(spec.symbol.value());
    }
    auto after = at->second;
    auto& pending =
        order.side == Side::bid ? after.pending_buy : after.pending_sell;
    pending = add(pending, order.quantity.value());
    const auto required = margin(after, spec);
    if (required > margin(at->second, spec)) {
        std::int64_t free{};
        for (const auto id : account.position_order.keys()) {
            const SymbolId symbol{id};
            const auto& p = account.positions.at(symbol);
            if (symbol == spec.symbol) {
                // 当前品种重置可用风险额度，遍历顺序会影响结果。
                free = estimate(p, spec, price(ledger.last_prices_, symbol));
            } else {
                const auto& other = lookup(symbol);
                if (other.quote == spec.quote) {
                    free = add(free,
                               sub(estimate(p, other,
                                            price(ledger.last_prices_, symbol)),
                                   margin(p, other)));
                }
            }
        }
        if (required > add(value(account.balances, spec.quote), free)) {
            if (created) {
                account.balances.try_emplace(spec.quote, 0);
                account.positions.erase(at);
                account.position_order.erase(spec.symbol.value());
            }
            return Status::insufficient_funds;
        }
    }
    at->second = after;
    return Status::processed;
}

std::int64_t FuturesRisk::free_margin(const Ledger& ledger, UserId user,
                                      CurrencyId currency,
                                      const SpecLookup& lookup) {
    const auto found = ledger.accounts_.find(user);
    if (found == ledger.accounts_.end()) {
        return 0;
    }
    std::int64_t total{};
    for (const auto& [symbol, p] : found->second.positions) {
        const auto& s = lookup(symbol);
        if (s.quote == currency) {
            total = add(total,
                        sub(estimate(p, s, price(ledger.last_prices_, symbol)),
                            margin(p, s)));
        }
    }
    return total;
}

Status SpotRisk::reserve(Ledger& ledger, const PlaceOrder& order,
                         const SymbolSpec& spec, const SpecLookup& lookup) {
    const auto account = ledger.accounts_.find(order.user);
    if (account == ledger.accounts_.end()) {
        return Status::unknown_user;
    }
    if (account->second.suspended) {
        return Status::user_suspended;
    }
    const Ledger::HoldKey key{order.key.symbol, order.key.order};
    if (ledger.holds_.contains(key)) {
        return Status::duplicate_order;
    }
    const auto hold = required_hold(order, spec);
    if (!hold || hold->units <= 0) {
        return Status::invalid_input;
    }
    const auto next =
        sub(value(account->second.balances, hold->currency), hold->units);
    if (add(next, FuturesRisk::free_margin(ledger, order.user, hold->currency,
                                           lookup)) < 0) {
        return Status::insufficient_funds;
    }
    ledger.holds_.try_emplace(key, Ledger::Hold{order.user, *hold});
    account->second.balances[hold->currency] = next;
    return Status::processed;
}

bool FuturesRisk::settle(Ledger& ledger, const PlaceOrder* placed,
                         OrderKey primary, const std::vector<Event>& events,
                         const Book& book, const SymbolSpec& spec) {
    const auto cleanup = [&](UserId user) {
        auto& a = ledger.accounts_.at(user);
        const auto it = a.positions.find(spec.symbol);
        if (it != a.positions.end() && it->second.empty()) {
            a.balances[spec.quote] =
                add(value(a.balances, spec.quote), it->second.profit);
            a.positions.erase(it);
            a.position_order.erase(spec.symbol.value());
        }
    };
    const auto release = [&](UserId user, Side side, Quantity qty) {
        auto& p = ledger.accounts_.at(user).positions.at(spec.symbol);
        auto& pending = side == Side::bid ? p.pending_buy : p.pending_sell;
        pending = sub(pending, qty.value());
        if (pending < 0) {
            throw std::logic_error{"negative released pending"};
        }
        cleanup(user);
    };
    const auto trade = [&](UserId user, Side side, const Trade& fill,
                           std::int64_t rate) {
        auto& a = ledger.accounts_.at(user);
        auto& p = a.positions.at(spec.symbol);
        const auto opened = update(p, side, fill.quantity, fill.price);
        const auto fee = mul(opened, rate);
        a.balances[spec.quote] = sub(value(a.balances, spec.quote), fee);
        ledger.fees_[spec.quote] = add(value(ledger.fees_, spec.quote), fee);
        cleanup(user);
    };
    // 先释放拒绝量，再按成交顺序结算。
    for (const auto& e : events) {
        if (const auto* reject = std::get_if<RejectRemainder>(&e)) {
            if (!placed) {
                return false;
            }
            release(reject->user, placed->side, reject->quantity);
        }
    }
    for (const auto& e : events) {
        if (const auto* fill = std::get_if<Trade>(&e)) {
            trade(fill->taker_user, fill->taker_side, *fill,
                  spec.taker_fee_per_lot);
            trade(fill->maker_user,
                  fill->taker_side == Side::bid ? Side::ask : Side::bid, *fill,
                  spec.maker_fee_per_lot);
        } else if (const auto* reduced = std::get_if<Reduce>(&e)) {
            release(reduced->user, reduced->side, reduced->quantity);
        }
    }
    if (placed && events.empty()) {
        static_cast<void>(primary);
    }
    // 从最终簿独立核对增量 pending，不能以重算代替释放协议。
    for (const auto& [user, a] : ledger.accounts_) {
        std::int64_t buy{}, sell{};
        for (const auto& o : book.resting_orders()) {
            if (o.user == user) {
                (o.side == Side::bid ? buy : sell) =
                    add(o.side == Side::bid ? buy : sell, o.remaining.value());
            }
        }
        const auto p = a.positions.find(spec.symbol);
        if (p == a.positions.end() ? buy != 0 || sell != 0
                                   : p->second.pending_buy != buy ||
                                         p->second.pending_sell != sell) {
            return false;
        }
    }
    return true;
}

void FuturesRisk::refresh(Ledger& ledger, SymbolId symbol, const Book& book) {
    const auto l2 = book.snapshot(1);
    ledger.last_prices_[symbol] =
        LastPrice{l2.asks.empty() ? std::numeric_limits<std::int64_t>::max()
                                  : l2.asks.front().price.value(),
                  l2.bids.empty() ? 0 : l2.bids.front().price.value()};
}

void FuturesRisk::reports(const Ledger& ledger, UserReport& report,
                          const SpecLookup& lookup) {
    const auto& a = ledger.accounts_.at(report.user);
    for (const auto& [symbol, p] : a.positions) {
        const auto& spec = lookup(symbol);
        report.positions.push_back(
            {symbol, spec.quote, p,
             estimate(p, spec, price(ledger.last_prices_, symbol)),
             margin(p, spec)});
    }
}

void FuturesRisk::totals(const Ledger& ledger, TotalsReport& report,
                         const SpecLookup& lookup) {
    const auto ja = [](std::int64_t a, std::int64_t b) {
        return std::bit_cast<std::int64_t>(std::uint64_t(a) + std::uint64_t(b));
    };
    const auto js = [](std::int64_t a, std::int64_t b) {
        return std::bit_cast<std::int64_t>(std::uint64_t(a) - std::uint64_t(b));
    };
    const auto jm = [](std::int64_t a, std::int64_t b) {
        return std::bit_cast<std::int64_t>(std::uint64_t(a) * std::uint64_t(b));
    };
    std::map<CurrencyId, std::int64_t> pnl;
    std::map<SymbolId, OpenInterest> interests;
    for (const auto& [user, a] : ledger.accounts_) {
        for (const auto& [symbol, p] : a.positions) {
            const auto& spec = lookup(symbol);
            LastPrice common{42, 42};
            if (const auto* last = price(ledger.last_prices_, symbol)) {
                const auto average = ja(last->ask, last->bid) >> 1;
                common = {average, average};
            }
            auto estimate = p.profit;
            if (p.signed_quantity > 0)
                estimate =
                    ja(estimate,
                       common.bid != 0
                           ? js(jm(p.signed_quantity, common.bid), p.cost_sum)
                           : jm(spec.margin_buy, p.signed_quantity));
            if (p.signed_quantity < 0)
                estimate = ja(
                    estimate,
                    common.ask != std::numeric_limits<std::int64_t>::max()
                        ? js(p.cost_sum,
                             jm(magnitude(p.signed_quantity), common.ask))
                        : jm(spec.margin_sell, magnitude(p.signed_quantity)));
            pnl[spec.quote] = ja(pnl[spec.quote], estimate);
            auto& oi = interests.try_emplace(symbol, OpenInterest{symbol})
                           .first->second;
            if (p.signed_quantity > 0)
                oi.long_volume = add(oi.long_volume, p.signed_quantity);
            if (p.signed_quantity < 0)
                oi.short_volume =
                    add(oi.short_volume, magnitude(p.signed_quantity));
        }
    }
    for (const auto& [currency, units] : pnl) {
        auto row =
            std::find_if(report.currencies.begin(), report.currencies.end(),
                         [&](const auto& r) { return r.currency == currency; });
        if (row == report.currencies.end()) {
            report.currencies.push_back(CurrencyTotals{currency});
            row = std::prev(report.currencies.end());
        }
        row->positions = units;
    }
    for (const auto& [symbol, oi] : interests) {
        report.open_interest.push_back(oi);
    }
}
}  // namespace exchange
