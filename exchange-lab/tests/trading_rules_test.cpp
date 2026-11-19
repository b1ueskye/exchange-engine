#include "trading_fixtures.hpp"
#include "test_checks.hpp"

#include <algorithm>
#include <iostream>
#include <unordered_set>
#include <utility>

namespace {

using namespace exchange;
using trading_fixture::PlaceFixture;

const RestingOrder* find_order(const std::vector<RestingOrder>& rows, OrderKey key) {
    const auto it = std::find_if(rows.begin(), rows.end(), [key](const RestingOrder& row) {
        return row.key == key;
    });
    return it == rows.end() ? nullptr : &*it;
}

const RestingOrder* best(const std::vector<RestingOrder>& rows, Side side) {
    const auto it = std::find_if(rows.begin(), rows.end(), [side](const RestingOrder& row) {
        return row.side == side;
    });
    return it == rows.end() ? nullptr : &*it;
}

bool canonical(const std::vector<RestingOrder>& rows, SymbolId symbol) {
    std::unordered_set<std::uint64_t> ids;
    const RestingOrder* prev = nullptr;
    for (const auto& row : rows) {
        if (row.key.symbol != symbol || row.key.order.value() == 0 ||
            row.user.value() == 0 || row.price.value() <= 0 ||
            row.remaining.value() <= 0 || row.priority_seq.value() == 0 ||
            (row.side != Side::ask && row.side != Side::bid) ||
            !ids.insert(row.key.order.value()).second) {
            return false;
        }
        if (prev && prev->side == Side::bid && row.side == Side::ask) {
            return false;
        }
        if (prev && prev->side == row.side) {
            if ((row.side == Side::ask && row.price < prev->price) ||
                (row.side == Side::bid && row.price > prev->price) ||
                (row.price == prev->price && row.priority_seq <= prev->priority_seq)) {
                return false;
            }
        }
        prev = &row;
    }
    const auto* bid = best(rows, Side::bid);
    const auto* ask = best(rows, Side::ask);
    return !bid || !ask || bid->price < ask->price;
}

bool check_place(const PlaceFixture& fixture) {
    const auto& cmd = fixture.command;
    const auto& before = fixture.before;
    const auto& after = fixture.after;
    if (!validate_command(cmd) || fixture.expected.status != Status::processed ||
        !canonical(before, cmd.key.symbol) || !canonical(after, cmd.key.symbol) ||
        find_order(before, cmd.key)) {
        return false;
    }

    const auto* limit = std::get_if<LimitPrice>(&cmd.constraint);
    const auto* budget = std::get_if<BudgetLimit>(&cmd.constraint);
    auto remaining = cmd.quantity.value();
    std::int64_t rejected = 0;
    std::int64_t total = 0;
    auto expected_after = before;

    for (const auto& event : fixture.expected.events) {
        if (const auto* trade = std::get_if<Trade>(&event)) {
            const Side maker_side = cmd.side == Side::bid ? Side::ask : Side::bid;
            const auto* maker = best(expected_after, maker_side);
            if (rejected != 0 || remaining <= 0 || !maker ||
                trade->taker != cmd.key || trade->taker_user != cmd.user ||
                trade->taker_side != cmd.side || trade->maker != maker->key ||
                trade->maker_user != maker->user || trade->price != maker->price) {
                return false;
            }
            if (limit && ((cmd.side == Side::bid && maker->price > limit->price) ||
                          (cmd.side == Side::ask && maker->price < limit->price))) {
                return false;
            }

            const auto amount = trade->quantity.value();
            if (amount != std::min(remaining, maker->remaining.value())) {
                return false;
            }
            remaining -= amount;
            const auto maker_remaining = maker->remaining.value() - amount;
            if (trade->maker_filled != (maker_remaining == 0) ||
                trade->taker_filled != (remaining == 0)) {
                return false;
            }

            const auto product = checked_multiply(amount, maker->price.value());
            if (!product) {
                return false;
            }
            const auto next_total = checked_add(total, *product);
            if (!next_total) {
                return false;
            }
            total = *next_total;

            const auto pos = std::find_if(expected_after.begin(), expected_after.end(),
                [trade](const RestingOrder& row) { return row.key == trade->maker; });
            if (maker_remaining == 0) {
                expected_after.erase(pos);
            } else {
                pos->remaining = Quantity{maker_remaining};
            }
        } else if (const auto* reject = std::get_if<RejectRemainder>(&event)) {
            if (rejected != 0 || remaining <= 0 || reject->key != cmd.key ||
                reject->user != cmd.user || reject->quantity.value() != remaining ||
                (limit && (limit->policy != LimitPolicy::ioc ||
                           reject->reason != RejectReason::ioc_remainder)) ||
                (budget && (remaining != cmd.quantity.value() ||
                            reject->reason == RejectReason::ioc_remainder))) {
                return false;
            }
            rejected = remaining;
            remaining = 0;
        } else {
            return false;
        }
    }

    const auto* resting = find_order(after, cmd.key);
    const auto* opposite = best(expected_after, cmd.side == Side::bid ? Side::ask : Side::bid);
    if (limit && opposite && (remaining > 0 || rejected > 0) &&
        ((cmd.side == Side::bid && opposite->price <= limit->price) ||
         (cmd.side == Side::ask && opposite->price >= limit->price))) {
        return false;
    }
    if (budget && (remaining != 0 || resting ||
        (rejected == 0 && (cmd.side == Side::bid ? total > budget->total.value()
                                               : total < budget->total.value())))) {
        return false;
    }
    if (remaining > 0) {
        if (!resting || !limit || limit->policy != LimitPolicy::gtc ||
            resting->user != cmd.user || resting->side != cmd.side ||
            resting->price != limit->price || resting->remaining != Quantity{remaining} ||
            resting->priority_seq != fixture.seq || resting->timestamp_ns != cmd.timestamp_ns ||
            resting->reserve_bid_price !=
                (cmd.side == Side::bid
                    ? std::optional<Price>{limit->reserve_bid_price.value_or(limit->price)}
                    : std::nullopt)) {
            return false;
        }
    } else if (resting) {
        return false;
    }

    auto after_without_taker = after;
    std::erase_if(after_without_taker, [&cmd](const RestingOrder& row) {
        return row.key == cmd.key;
    });
    return total == fixture.executed_steps && expected_after == after_without_taker;
}

}

int main() {
    using namespace exchange;
    TestChecks checks;
    const auto fixtures = trading_fixture::cases();
    for (const auto& fixture : fixtures) {
        checks.require(check_place(fixture), fixture.name);
    }

    const auto move = trading_fixture::same_price_move();
    checks.require(validate_command(move.command) &&
                   canonical(move.before, move.command.key.symbol) &&
                   canonical(move.after, move.command.key.symbol) &&
                   move.expected.status == Status::processed && move.expected.events.empty() &&
                   move.before.size() == 2 && move.after.size() == 2 &&
                   move.after.at(0) == move.before.at(1), "same-price move preserves other order");
    if (move.before.size() == 2 && move.after.size() == 2) {
        auto moved = move.before.at(0);
        moved.priority_seq = move.seq;
        checks.require(move.after.at(1) == moved, "same-price move joins the tail");
    }

    auto wrong = fixtures.at(0);
    std::swap(wrong.expected.events.at(0), wrong.expected.events.at(1));
    checks.require(!check_place(wrong), "oracle detects wrong FIFO");
    wrong = fixtures.at(2);
    wrong.expected.events.pop_back();
    checks.require(!check_place(wrong), "oracle detects missing IOC rejection");
    wrong = fixtures.at(4);
    wrong.after.erase(wrong.after.begin());
    checks.require(!check_place(wrong), "oracle detects mutation on FOK rejection");

    if (checks.exit_code() == 0) {
        std::cout << fixtures.size() << " place fixtures and 1 move fixture checked\n";
    }
    return checks.exit_code();
}