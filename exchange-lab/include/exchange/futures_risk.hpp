#ifndef EXCHANGE_FUTURES_RISK_HPP
#define EXCHANGE_FUTURES_RISK_HPP
#include "exchange/ledger.hpp"
#include "exchange/order_book.hpp"
#include <functional>

namespace exchange {
using SpecLookup = std::function<const SymbolSpec&(SymbolId)>;

struct FuturesRisk {
    static std::int64_t margin(const MarginPosition&, const SymbolSpec&);
    static std::int64_t estimate(const MarginPosition&, const SymbolSpec&,
                                 const LastPrice*);
    static std::int64_t update(MarginPosition&, Side, Quantity, Price);
    static Status reserve(Ledger&, const PlaceOrder&, const SymbolSpec&,
                          const SpecLookup&);
    static bool settle(Ledger&, const PlaceOrder* placed, OrderKey primary,
                       const std::vector<Event>&, const Book&,
                       const SymbolSpec&);
    static std::int64_t free_margin(const Ledger&, UserId, CurrencyId,
                                    const SpecLookup&);
    static void refresh(Ledger&, SymbolId, const Book&);
    static void reports(const Ledger&, UserReport&, const SpecLookup&);
    static void totals(const Ledger&, TotalsReport&, const SpecLookup&);
};

struct SpotRisk {
    static Status reserve(Ledger&, const PlaceOrder&, const SymbolSpec&,
                          const SpecLookup&);
    static bool settle(Ledger& ledger, OrderKey key,
                       const std::vector<Event>& events, const Book& book,
                       const SymbolSpec& spec) {
        return ledger.apply_spot(key, events, book, spec);
    }
};
}  // namespace exchange
#endif
