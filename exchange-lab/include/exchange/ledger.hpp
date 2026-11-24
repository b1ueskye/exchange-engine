#ifndef EXCHANGE_LEDGER_HPP
#define EXCHANGE_LEDGER_HPP

#include "exchange/model.hpp"

#include <map>
#include <utility>

namespace exchange {

class Book;

std::optional<Money> required_hold(const PlaceOrder& order,
                                   const SymbolSpec& spec);

class Ledger {
public:
    Status add_user(UserId user);
    Status adjust(const AdjustBalance& command);
    Status suspend(UserId user);
    Status resume(UserId user);
    Status freeze(const PlaceOrder& order, const SymbolSpec& spec);

    std::optional<Money> available(UserId user, CurrencyId currency) const;
    std::optional<Money> held(OrderKey key) const;
    Money fees(CurrencyId currency) const;
    void swap(Ledger& other) noexcept;
    std::optional<UserReport> user_report(UserId user) const;
    TotalsReport totals() const;
    void append_canonical(std::string& out) const;

private:
    friend class Engine;

    bool consume(OrderKey key, UserId user, Money amount);
    bool credit(UserId user, Money amount);
    bool apply_trade(const Trade& trade, const SymbolSpec& spec);
    bool align_hold(OrderKey key, const Book& book, const SymbolSpec& spec);
    bool apply_spot(OrderKey primary, const std::vector<Event>& events,
                    const Book& book, const SymbolSpec& spec);

    struct Account {
        std::map<CurrencyId, std::int64_t> balances;
        std::uint64_t last_funding{};
        bool suspended{};
    };

    struct Hold {
        UserId user;
        Money money;
    };

    using HoldKey = std::pair<SymbolId, OrderId>;

    std::map<UserId, Account> accounts_;
    std::map<HoldKey, Hold> holds_;
    std::map<CurrencyId, std::int64_t> net_deposits_;
    std::map<CurrencyId, std::int64_t> fees_;
};

}  // namespace exchange

#endif
