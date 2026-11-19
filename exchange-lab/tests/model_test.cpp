
#include "exchange/model.hpp"
#include "test_checks.hpp"

#include <iostream>
#include <type_traits>

int main() {
    using namespace exchange;
    static_assert(!std::is_convertible_v<Seq, OrderId>);

    TestChecks checks;
    const OrderKey taker{.symbol = SymbolId{1}, .order = OrderId{90}};
    const OrderKey a{SymbolId{1}, OrderId{7}};
    const OrderKey b{SymbolId{1}, OrderId{8}};
    const UserId buyer{30};
    const PlaceOrder ioc{taker, buyer, Side::bid, Quantity{6},
                         LimitPrice{Price{102}, LimitPolicy::ioc, Price{105}}, 10};
    checks.require(validate_command(ioc), "valid IOC");
    checks.require(validate_command(CancelOrder{taker, buyer}), "valid cancel");
    checks.require(validate_command(MoveOrder{taker, buyer, Price{101}}), "valid move");
    checks.require(!validate_command(ReduceOrder{taker, buyer, Quantity{0}}),
                   "zero reduce");
    checks.require(!validate_command(CancelOrder{taker, UserId{0}}), "missing owner");

    auto invalid = ioc;
    invalid.constraint = LimitPrice{Price{102}, LimitPolicy::ioc, Price{101}};
    checks.require(!validate_command(invalid), "reserve below bid limit");
    invalid = ioc;
    invalid.side = Side::ask;
    checks.require(!validate_command(invalid), "ask cannot carry bid reserve");
    invalid.constraint = BudgetLimit{NotionalSteps{608}};
    checks.require(validate_command(invalid), "valid budget ask");
    invalid.constraint = BudgetLimit{NotionalSteps{0}};
    checks.require(!validate_command(invalid), "zero budget");

    const Result result{
        Seq{17},
        {Status::processed, {
            Trade{a, UserId{11}, taker, buyer, Side::bid, Price{101}, Quantity{3}, true, false},
            Trade{b, UserId{12}, taker, buyer, Side::bid, Price{101}, Quantity{2}, true, false},
            RejectRemainder{taker, buyer, Quantity{1}, RejectReason::ioc_remainder},
        }},
    };
    const auto* second = std::get_if<Trade>(&result.outcome.events.at(1));
    checks.require(second && second->maker == b && second->maker_filled &&
                   !second->taker_filled, "trade completion flags");
    checks.require(describe_result(result) ==
                   "seq=17 status=processed\n"
                   "trade maker=7 taker=90 price=101 qty=3\n"
                   "trade maker=8 taker=90 price=101 qty=2\n"
                   "reject order=90 qty=1 reason=ioc_remainder", "event order");

    const Event reduced = Reduce{taker, buyer, Side::bid, Price{101}, Quantity{2}, true};
    checks.require(describe_event(reduced) == "reduce order=90 qty=2", "reduce formatting");

    auto independent_copy = result;
    independent_copy.outcome.events.clear();
    checks.require(result.outcome.events.size() == 3, "result owns its events");
    std::cout << describe_result(result) << '\n';
    return checks.exit_code();
}