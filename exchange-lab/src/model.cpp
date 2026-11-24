#include "exchange/model.hpp"

#include <string>

namespace exchange {

namespace {

bool valid_owner(const OrderKey& key, UserId user) {
    return key.symbol.value() > 0 && key.order.value() > 0 && user.value() > 0;
}

struct CheckCommand {
    bool operator()(const RegisterSymbol& cmd) const {
        return valid_symbol(cmd.spec);
    }

    bool operator()(const PlaceOrder& cmd) const {
        if (!valid_owner(cmd.key, cmd.user) || cmd.quantity.value() <= 0 ||
            (cmd.side != Side::bid && cmd.side != Side::ask) ||
            cmd.timestamp_ns < 0) {
            return false;
        }

        if (const auto* limit = std::get_if<LimitPrice>(&cmd.constraint)) {
            if (limit->price.value() <= 0 ||
                (limit->policy != LimitPolicy::gtc &&
                 limit->policy != LimitPolicy::ioc)) {
                return false;
            }
            return !limit->reserve_bid_price ||
                   (cmd.side == Side::bid &&
                    *limit->reserve_bid_price >= limit->price);
        }
        const auto* budget = std::get_if<BudgetLimit>(&cmd.constraint);
        return budget && budget->total.value() > 0;
    }

    bool operator()(const CancelOrder& cmd) const {
        return valid_owner(cmd.key, cmd.user);
    }

    bool operator()(const ReduceOrder& cmd) const {
        return valid_owner(cmd.key, cmd.user) && cmd.quantity.value() > 0;
    }

    bool operator()(const MoveOrder& cmd) const {
        return valid_owner(cmd.key, cmd.user) && cmd.new_price.value() > 0;
    }

    bool operator()(const AddUser& cmd) const {
        return cmd.user.value() > 0;
    }

    bool operator()(const AdjustBalance& cmd) const {
        return cmd.user.value() > 0 && cmd.amount.currency.value() > 0 &&
               cmd.funding_id > 0 && cmd.amount.units != 0;
    }

    bool operator()(const SuspendUser& cmd) const {
        return cmd.user.value() > 0;
    }

    bool operator()(const ResumeUser& cmd) const {
        return cmd.user.value() > 0;
    }
};

const char* status_name(Status status) {
    switch (status) {
        case Status::processed:
            return "processed";
        case Status::invalid_input:
            return "invalid_input";
        case Status::duplicate_order:
            return "duplicate_order";
        case Status::unknown_symbol:
            return "unknown_symbol";
        case Status::unknown_order:
            return "unknown_order";
        case Status::insufficient_funds:
            return "insufficient_funds";
        case Status::price_over_reserve:
            return "price_over_reserve";
        case Status::symbol_exists:
            return "symbol_exists";
        case Status::unknown_user:
            return "unknown_user";
        case Status::user_suspended:
            return "user_suspended";
        case Status::user_exists:
            return "user_exists";
        case Status::account_not_empty:
            return "account_not_empty";
        case Status::duplicate_funding:
            return "duplicate_funding";
        case Status::stale_funding:
            return "stale_funding";
        case Status::settlement_failed:
            return "settlement_failed";
    }
    return "unknown_status";
}

const char* reason_name(RejectReason reason) {
    switch (reason) {
        case RejectReason::ioc_remainder:
            return "ioc_remainder";
        case RejectReason::insufficient_liquidity:
            return "insufficient_liquidity";
        case RejectReason::budget_limit:
            return "budget_limit";
        case RejectReason::arithmetic_limit:
            return "arithmetic_limit";
    }
    return "unknown_reason";
}

struct DescribeEvent {
    std::string operator()(const Trade& trade) const {
        return "trade maker=" + std::to_string(trade.maker.order.value()) +
               " taker=" + std::to_string(trade.taker.order.value()) +
               " price=" + std::to_string(trade.price.value()) +
               " qty=" + std::to_string(trade.quantity.value());
    }

    std::string operator()(const Reduce& reduce) const {
        return "reduce order=" + std::to_string(reduce.key.order.value()) +
               " qty=" + std::to_string(reduce.quantity.value());
    }

    std::string operator()(const RejectRemainder& reject) const {
        return "reject order=" + std::to_string(reject.key.order.value()) +
               " qty=" + std::to_string(reject.quantity.value()) +
               " reason=" + reason_name(reject.reason);
    }
};

}  // namespace

bool validate_command(const Command& command) {
    return std::visit(CheckCommand{}, command);
}

std::string describe_event(const Event& event) {
    return std::visit(DescribeEvent{}, event);
}

std::string describe_result(const Result& result) {
    std::string text = "seq=" + std::to_string(result.seq.value()) +
                       " status=" + status_name(result.outcome.status);
    for (const auto& event : result.outcome.events) {
        text += '\n';
        text += describe_event(event);
    }
    return text;
}

}  // namespace exchange
