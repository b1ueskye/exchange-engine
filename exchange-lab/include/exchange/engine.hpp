#ifndef EXCHANGE_ENGINE_HPP
#define EXCHANGE_ENGINE_HPP

#include "exchange/order_book.hpp"

#include <map>

namespace exchange {

class SymbolCatalog {
private:
    friend class Engine;
    struct Market {
        SymbolSpec spec;
        std::unique_ptr<OrderBook> book;
    };

    Status add(const SymbolSpec& spec);
    std::map<SymbolId, Market> markets_;
};

class Engine {
public:
    Result execute(const Command& command);
    std::optional<std::vector<RestingOrder>> book_orders(SymbolId symbol) const;

private:
    CommandResult dispatch(const RegisterSymbol& command, Seq seq);

    template <class T>
    CommandResult dispatch(const T& command, Seq seq);

    SymbolCatalog catalog_;
    std::uint64_t last_seq_{};
};

}  // namespace exchange

#endif