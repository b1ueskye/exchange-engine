#ifndef EXCHANGE_ORDER_DRAFT_HPP
#define EXCHANGE_ORDER_DRAFT_HPP

#include <cstdint>
#include <optional>

namespace exchange {

enum class Side { bid, ask };

class OrderDraft {
public:
    static std::optional<OrderDraft> create(std::int64_t id, Side side,
                                            std::int64_t price,
                                            std::int64_t quantity) {
        if (id <= 0 || price <= 0 || quantity <= 0 ||
            (side != Side::bid && side != Side::ask)) {
            return std::nullopt;
        }
        return OrderDraft{id, side, price, quantity};
    }

    std::int64_t id() const {
        return id_;
    }
    Side side() const {
        return side_;
    }
    std::int64_t price() const {
        return price_;
    }
    std::int64_t quantity() const {
        return quantity_;
    }

    bool set_quantity(std::int64_t quantity) {
        if (quantity <= 0) {
            return false;
        }
        quantity_ = quantity;
        return true;
    }

private:
    OrderDraft(std::int64_t id, Side side, std::int64_t price,
               std::int64_t quantity)
        : id_(id), side_(side), price_(price), quantity_(quantity) {}
    std::int64_t id_;
    Side side_;
    std::int64_t price_;
    std::int64_t quantity_;
};

}  // namespace exchange

#endif