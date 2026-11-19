#ifndef EXCHANGE_TRADING_FIXTURES_HPP
#define EXCHANGE_TRADING_FIXTURES_HPP

#include "exchange/model.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace trading_fixture {

using namespace exchange;

struct PlaceFixture {
    const char* name;
    Seq seq;
    std::vector<RestingOrder> before;
    PlaceOrder command;
    CommandResult expected;
    std::vector<RestingOrder> after;
    std::int64_t executed_steps;
};

struct MoveFixture {
    Seq seq;
    std::vector<RestingOrder> before;
    MoveOrder command;
    CommandResult expected;
    std::vector<RestingOrder> after;
};

inline OrderKey key(std::uint64_t id) {
    return {SymbolId{1}, OrderId{id}};
}

inline RestingOrder rest(std::uint64_t id, Side side, std::int64_t price,
                         std::int64_t quantity, std::uint64_t seq, std::int64_t timestamp) {
    std::optional<Price> reserve;
    if (side == Side::bid) {
        reserve = Price{price};
    }
    return {key(id), UserId{id}, side, Price{price}, Quantity{quantity},
            reserve, Seq{seq}, timestamp};
}

inline PlaceOrder limit(Side side, std::int64_t price, std::int64_t quantity,
                        LimitPolicy policy) {
    return {key(90), UserId{90}, side, Quantity{quantity},
            LimitPrice{Price{price}, policy, std::nullopt}, 400};
}

inline PlaceOrder budget(Side side, std::int64_t total, std::int64_t quantity) {
    return {key(90), UserId{90}, side, Quantity{quantity},
            BudgetLimit{NotionalSteps{total}}, 400};
}

inline Trade fill(const RestingOrder& maker, const PlaceOrder& taker,
                  std::int64_t quantity, bool maker_filled, bool taker_filled) {
    return {maker.key, maker.user, taker.key, taker.user, taker.side, maker.price,
            Quantity{quantity}, maker_filled, taker_filled};
}

inline std::vector<PlaceFixture> cases() {
    const auto a = rest(7, Side::ask, 101, 3, 1, 300);
    const auto b = rest(8, Side::ask, 101, 2, 2, 200);
    const auto c = rest(9, Side::ask, 103, 4, 3, 250);
    const std::vector<RestingOrder> asks{a, b, c};

    const auto four = limit(Side::bid, 102, 4, LimitPolicy::gtc);
    const auto six = limit(Side::bid, 102, 6, LimitPolicy::gtc);
    const auto ioc = limit(Side::bid, 102, 6, LimitPolicy::ioc);
    const auto buy_fok = budget(Side::bid, 608, 6);
    const auto low_budget = budget(Side::bid, 607, 6);

    const auto x = rest(10, Side::bid, 105, 3, 1, 300);
    const auto y = rest(11, Side::bid, 103, 2, 2, 200);
    const std::vector<RestingOrder> bids{x, y};
    const auto sell_fok = budget(Side::ask, 418, 4);
    const auto high_minimum = budget(Side::ask, 419, 4);

    return {
        {"gtc_fifo", Seq{4}, asks, four,
         {Status::processed, {fill(a, four, 3, true, false), fill(b, four, 1, false, true)}},
         {rest(8, Side::ask, 101, 1, 2, 200), c}, 404},
        {"gtc_rests", Seq{4}, asks, six,
         {Status::processed, {fill(a, six, 3, true, false), fill(b, six, 2, true, false)}},
         {c, rest(90, Side::bid, 102, 1, 4, 400)}, 505},
        {"ioc_remainder", Seq{4}, asks, ioc,
         {Status::processed, {fill(a, ioc, 3, true, false), fill(b, ioc, 2, true, false),
                              RejectRemainder{ioc.key, ioc.user, Quantity{1},
                                              RejectReason::ioc_remainder}}},
         {c}, 505},
        {"buy_fok_exact", Seq{4}, asks, buy_fok,
         {Status::processed, {fill(a, buy_fok, 3, true, false), fill(b, buy_fok, 2, true, false),
                              fill(c, buy_fok, 1, false, true)}},
         {rest(9, Side::ask, 103, 3, 3, 250)}, 608},
        {"buy_fok_short_budget", Seq{4}, asks, low_budget,
         {Status::processed, {RejectRemainder{low_budget.key, low_budget.user, Quantity{6},
                                              RejectReason::budget_limit}}},
         asks, 0},
        {"sell_fok_exact", Seq{3}, bids, sell_fok,
         {Status::processed, {fill(x, sell_fok, 3, true, false), fill(y, sell_fok, 1, false, true)}},
         {rest(11, Side::bid, 103, 1, 2, 200)}, 418},
        {"sell_fok_high_minimum", Seq{3}, bids, high_minimum,
         {Status::processed, {RejectRemainder{high_minimum.key, high_minimum.user, Quantity{4},
                                              RejectReason::budget_limit}}},
         bids, 0},
    };
}

inline MoveFixture same_price_move() {
    const auto a = rest(7, Side::ask, 101, 3, 1, 300);
    const auto b = rest(8, Side::ask, 101, 2, 2, 200);
    return {Seq{3}, {a, b}, {a.key, a.user, Price{101}}, {Status::processed, {}},
            {b, rest(7, Side::ask, 101, 3, 3, 300)}};
}

}

#endif