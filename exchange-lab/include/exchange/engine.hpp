#ifndef EXCHANGE_ENGINE_HPP
#define EXCHANGE_ENGINE_HPP

#include "exchange/order_book.hpp"
#include "exchange/ledger.hpp"

#include <map>

namespace exchange {

class SymbolCatalog {
private:
    friend class Engine;
    struct Market {
        SymbolSpec spec;
        std::unique_ptr<Book> book;
    };

    Status add(const SymbolSpec& spec, BookKind kind, std::size_t capacity);
    std::map<SymbolId, Market> markets_;
};

class Engine {
public:
    explicit Engine(BookKind kind = BookKind::list, std::size_t capacity = 4096)
        : book_kind_(kind), pool_capacity_(capacity) {}
    Result execute(const Command& command);
    Seq last_sequence() const {
        return Seq{last_seq_};
    }
    Seq next_sequence() const;
    Result execute_at(const Command& command, Seq seq);
    std::optional<std::vector<RestingOrder>> book_orders(SymbolId symbol) const;
    const Ledger& ledger() const {
        return ledger_;
    }

private:
    CommandResult dispatch(const RegisterSymbols& command, Seq seq);
    CommandResult dispatch(const AddUsers& command, Seq seq);

    Result answer(const QueryUser& command, Seq seq);
    Result answer(const QueryTotals& command, Seq seq);
    Result answer(const QueryFingerprint& command, Seq seq);

    template <class T>
    Result answer(const T& command, Seq seq) {
        return {seq, dispatch(command, seq)};
    }

    CommandResult dispatch(const RegisterSymbol& command, Seq seq);
    CommandResult dispatch(const AddUser& command, Seq seq);
    CommandResult dispatch(const AdjustBalance& command, Seq seq);
    CommandResult dispatch(const SuspendUser& command, Seq seq);
    CommandResult dispatch(const ResumeUser& command, Seq seq);

    template <class T>
    CommandResult dispatch(const T& command, Seq seq);

    SymbolCatalog catalog_;
    Ledger ledger_;
    std::uint64_t last_seq_{};
    BookKind book_kind_;
    std::size_t pool_capacity_;
};

}  // namespace exchange

#endif
