#ifndef EXCHANGE_LEDGER_HPP
#define EXCHANGE_LEDGER_HPP

#include "exchange/model.hpp"

#include <map>
#include <utility>

namespace exchange {

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
    void swap(Ledger& other) noexcept;

private:
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
};

}  // namespace exchange

#endif