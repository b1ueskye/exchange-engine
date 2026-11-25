#ifndef EXCHANGE_SPOT_UPDATE_HPP
#define EXCHANGE_SPOT_UPDATE_HPP

#include "exchange/ledger.hpp"
#include "exchange/order_book.hpp"

#include <array>

namespace exchange {

class Ledger::SpotUpdate {
public:
    SpotUpdate(Ledger& ledger, OrderKey primary, const SymbolSpec& spec);
    Status freeze(const PlaceOrder& order);
    bool prepare(const OrderBook& book, const std::vector<Event>& events,
                 const std::optional<RestingOrder>& primary_after);
    void commit();

private:
    using Amounts = std::map<CurrencyId, std::int64_t>;
    using BalanceKey = std::pair<UserId, CurrencyId>;

    struct AmountChange {
        AmountChange(Amounts& map, CurrencyId currency, std::int64_t* known);
        void commit();

        Amounts* target;
        std::int64_t* existing;
        Amounts::node_type pending;
        std::int64_t value;
    };

    struct MakerChange {
        Holds::iterator existing;
        Hold hold;
        const Trade* trade;
    };

    AmountChange& balance(UserId user, Amounts& amounts, CurrencyId currency,
                          std::int64_t* known = nullptr);
    bool credit(UserId user, Money amount);
    static bool consume(Hold& hold, UserId user, Money amount);
    bool apply_trade(const Trade& trade, Hold& maker);
    bool align_hold(Hold& hold, const std::optional<RestingOrder>& order);

    Ledger& ledger_;
    HoldKey primary_key_;
    const SymbolSpec& spec_;
    Holds::iterator primary_existing_;
    std::optional<Hold> primary_;
    Holds::node_type primary_pending_;
    std::optional<AmountChange> fee_;
    std::array<std::optional<std::pair<BalanceKey, AmountChange>>, 4> inline_balances_;
    std::size_t inline_count_{};
    std::map<BalanceKey, AmountChange> extra_balances_;
    std::vector<MakerChange> makers_;
};

}  // namespace exchange

#endif
