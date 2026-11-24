#include "reference_book.hpp"
#include "test_checks.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <utility>

namespace {

using namespace exchange;

struct Input {
    Seq seq;
    Command command;
};

enum class Difference { result, orders, l2, invariant };

struct Failure {
    std::size_t offset;
    Difference kind;
    std::string details;
};

std::vector<Input> generate(std::uint32_t seed, std::size_t size) {
    std::mt19937 rng{seed};
    std::vector<Input> commands;
    commands.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        const Seq seq{static_cast<std::uint64_t>(i) + 1};
        const OrderKey key{SymbolId{1}, OrderId{1 + rng() % 24}};
        const auto mode = rng() % 6;
        const auto direction = rng() % 2;
        const Side side = direction == 0 ? Side::bid : Side::ask;
        const auto price_value = static_cast<std::int64_t>(98 + rng() % 7);
        const Price price{price_value};
        const Quantity quantity{static_cast<std::int64_t>(rng() % 7)};
        const auto budget_value = static_cast<std::int64_t>(90 + rng() % 541);
        const auto timestamp = static_cast<std::int64_t>(rng() % 100);
        const auto user_selector = rng() % 4;
        const UserId owner{user_selector == 0 ? 99 : 1 + key.order.value() % 3};
        const UserId author{1 + key.order.value() % 3};
        switch (mode) {
            case 0:
                commands.push_back(
                    {seq, PlaceOrder{
                              key, author, side, quantity,
                              LimitPrice{price, LimitPolicy::gtc, std::nullopt},
                              timestamp}});
                break;
            case 1:
                commands.push_back(
                    {seq, PlaceOrder{
                              key, author, side, quantity,
                              LimitPrice{price, LimitPolicy::ioc, std::nullopt},
                              timestamp}});
                break;
            case 2:
                commands.push_back(
                    {seq, PlaceOrder{key, author, side, quantity,
                                     BudgetLimit{NotionalSteps{budget_value}},
                                     timestamp}});
                break;
            case 3:
                commands.push_back({seq, CancelOrder{key, owner}});
                break;
            case 4:
                commands.push_back({seq, ReduceOrder{key, owner, quantity}});
                break;
            case 5:
                commands.push_back({seq, MoveOrder{key, owner, price}});
                break;
        }
    }
    return commands;
}

CommandResult execute_book(Book& book, const Input& input) {
    if (const auto* place = std::get_if<PlaceOrder>(&input.command)) {
        return book.place(*place, input.seq);
    }
    if (const auto* cancel = std::get_if<CancelOrder>(&input.command)) {
        return book.cancel(*cancel);
    }
    if (const auto* reduce = std::get_if<ReduceOrder>(&input.command)) {
        return book.reduce(*reduce);
    }
    return book.move(std::get<MoveOrder>(input.command), input.seq);
}

std::string describe_row(const RestingOrder& row) {
    const auto reserve = row.reserve_bid_price
                             ? std::to_string(row.reserve_bid_price->value())
                             : "-";
    return "id=" + std::to_string(row.key.order.value()) +
           " user=" + std::to_string(row.user.value()) +
           " side=" + (row.side == Side::bid ? "bid" : "ask") +
           " price=" + std::to_string(row.price.value()) +
           " qty=" + std::to_string(row.remaining.value()) +
           " priority=" + std::to_string(row.priority_seq.value()) +
           " reserve=" + reserve + " ts=" + std::to_string(row.timestamp_ns);
}

std::optional<Failure> compare(const std::vector<Input>& inputs,
                               BookKind kind) {
    auto actual = make_book(kind, SymbolId{1}, 64);
    matching_test::ReferenceBook expected;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& input = inputs[i];
        const auto lhs = execute_book(*actual, input);
        const auto rhs = expected.execute(input.command, input.seq);
        if (lhs != rhs) {
            const auto [a, b] =
                std::mismatch(lhs.events.begin(), lhs.events.end(),
                              rhs.events.begin(), rhs.events.end());
            const auto label = [](const Event& event) {
                auto text = describe_event(event);
                if (const auto* trade = std::get_if<Trade>(&event)) {
                    text +=
                        " maker_user=" +
                        std::to_string(trade->maker_user.value()) +
                        " taker_user=" +
                        std::to_string(trade->taker_user.value()) + " side=" +
                        (trade->taker_side == Side::bid ? "bid" : "ask") +
                        " maker_filled=" + std::to_string(trade->maker_filled) +
                        " taker_filled=" + std::to_string(trade->taker_filled);
                } else if (const auto* cut = std::get_if<Reduce>(&event)) {
                    text +=
                        " user=" + std::to_string(cut->user.value()) +
                        " side=" + (cut->side == Side::bid ? "bid" : "ask") +
                        " price=" + std::to_string(cut->price.value()) +
                        " removed=" + std::to_string(cut->removed);
                } else {
                    text += " user=" +
                            std::to_string(
                                std::get<RejectRemainder>(event).user.value());
                }
                return text;
            };
            const auto first_actual = a == lhs.events.end() ? "end" : label(*a);
            const auto first_expected =
                b == rhs.events.end() ? "end" : label(*b);
            return Failure{i, Difference::result,
                           "actual " + describe_result({input.seq, lhs}) +
                               "\nexpected " +
                               describe_result({input.seq, rhs}) +
                               "\nfirst differing events\nactual " +
                               first_actual + "\nexpected " + first_expected};
        }

        const auto left = actual->resting_orders();
        const auto right = expected.canonical();
        const auto [a, b] =
            std::mismatch(left.begin(), left.end(), right.begin(), right.end());
        if (a != left.end() || b != right.end()) {
            const auto first_actual =
                a == left.end() ? "end" : describe_row(*a);
            const auto first_expected =
                b == right.end() ? "end" : describe_row(*b);
            return Failure{
                i, Difference::orders,
                "actual " + first_actual + "\nexpected " + first_expected};
        }

        const auto l2 = actual->snapshot(3);
        const auto reference_l2 = expected.snapshot(3);
        if (l2 != reference_l2) {
            const auto explain = [](const auto& levels) {
                std::string text;
                for (const auto& level : levels) {
                    text += " " + std::to_string(level.price.value()) + "/" +
                            std::to_string(level.total_remaining.value()) +
                            "/" + std::to_string(level.order_count);
                }
                return text;
            };
            return Failure{i, Difference::l2,
                           "actual asks" + explain(l2.asks) + " bids" +
                               explain(l2.bids) + "\nexpected asks" +
                               explain(reference_l2.asks) + " bids" +
                               explain(reference_l2.bids)};
        }
        if (!actual->validate()) {
            return Failure{i, Difference::invariant, "book invariant failed"};
        }
    }
    return std::nullopt;
}

std::vector<Input> minimize(std::vector<Input> inputs, const Failure& first,
                            BookKind kind) {
    inputs.erase(inputs.begin() + static_cast<std::ptrdiff_t>(first.offset + 1),
                 inputs.end());
    for (std::size_t i = 0; i < inputs.size();) {
        auto candidate = inputs;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(i));
        const auto failure = compare(candidate, kind);
        if (failure && failure->kind == first.kind) {
            inputs = std::move(candidate);
            i = 0;
        } else {
            ++i;
        }
    }
    return inputs;
}

void print_input(const Input& input) {
    std::cout << "seq=" << input.seq.value() << ' ';
    const auto describe = [](OrderKey key, UserId user) {
        std::cout << "id=" << key.order.value() << " user=" << user.value();
    };
    if (const auto* place = std::get_if<PlaceOrder>(&input.command)) {
        std::cout << "place ";
        describe(place->key, place->user);
        std::cout << " side=" << (place->side == Side::bid ? "bid" : "ask")
                  << " qty=" << place->quantity.value()
                  << " ts=" << place->timestamp_ns;
        if (const auto* limit = std::get_if<LimitPrice>(&place->constraint)) {
            std::cout << " limit=" << limit->price.value() << " policy="
                      << (limit->policy == LimitPolicy::gtc ? "gtc" : "ioc");
        } else {
            std::cout << " budget="
                      << std::get<BudgetLimit>(place->constraint).total.value();
        }
    } else if (const auto* cancel = std::get_if<CancelOrder>(&input.command)) {
        std::cout << "cancel ";
        describe(cancel->key, cancel->user);
    } else if (const auto* reduce = std::get_if<ReduceOrder>(&input.command)) {
        std::cout << "reduce ";
        describe(reduce->key, reduce->user);
        std::cout << " qty=" << reduce->quantity.value();
    } else {
        const auto& move = std::get<MoveOrder>(input.command);
        std::cout << "move ";
        describe(move.key, move.user);
        std::cout << " price=" << move.new_price.value();
    }
    std::cout << '\n';
}

}  // namespace

int main() {
    constexpr std::size_t steps = 1200;
    for (const auto kind : {BookKind::list, BookKind::pooled}) {
        const char* name = kind == BookKind::list ? "list" : "pooled";
        for (const std::uint32_t seed : {7u, 31u, 2026u}) {
            auto inputs = generate(seed, steps);
            const auto failure = compare(inputs, kind);
            if (failure) {
                std::cerr << "book=" << name << " seed=" << seed
                          << " command index=" << failure->offset
                          << " seq=" << inputs.at(failure->offset).seq.value()
                          << '\n'
                          << failure->details << '\n';
                const auto reduced =
                    minimize(std::move(inputs), *failure, kind);
                std::cout << "reduced sequence: " << reduced.size()
                          << " commands\n";
                for (const auto& input : reduced) {
                    print_input(input);
                }
                return 1;
            }
        }
        std::cout << "book=" << name
                  << " 3600 commands checked against the reference book\n";
    }
}
