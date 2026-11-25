#include "router.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <charconv>
#include <memory>
#include <stdexcept>

namespace exchange::api {
namespace {

struct JsonDeleter {
    void operator()(json_object* value) const {
        json_object_put(value);
    }
};
using Json = std::unique_ptr<json_object, JsonDeleter>;

Json own(json_object* value) {
    if (!value) throw std::bad_alloc{};
    return Json{value};
}

Json object() {
    return own(json_object_new_object());
}
Json array() {
    return own(json_object_new_array());
}

void put(const Json& target, const char* name, Json value) {
    if (json_object_object_add(target.get(), name, value.get()) != 0) {
        throw std::bad_alloc{};
    }
    value.release();
}

void append(const Json& target, Json value) {
    if (json_object_array_add(target.get(), value.get()) != 0) {
        throw std::bad_alloc{};
    }
    value.release();
}

Json text(std::string_view value) {
    return own(json_object_new_string_len(value.data(),
                                          static_cast<int>(value.size())));
}

template <class Int>
Json integer(Int value) {
    return text(std::to_string(value));
}

Json flag(bool value) {
    return own(json_object_new_boolean(value));
}

Response reply(int status, const Json& body) {
    const auto* serialized =
        json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN);
    if (!serialized) throw std::bad_alloc{};
    return {status, serialized};
}

class InvalidRequest : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Json parse_object(std::string_view body) {
    // Supply a terminal NUL so the tokener can finalize all JSON value types.
    const std::string source{body};
    const std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{
        json_tokener_new_ex(16), &json_tokener_free};
    if (!parser) throw std::bad_alloc{};
    json_tokener_set_flags(parser.get(),
                           JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value{json_tokener_parse_ex(parser.get(), source.c_str(),
                                     static_cast<int>(source.size() + 1))};
    if (json_tokener_get_error(parser.get()) != json_tokener_success ||
        json_tokener_get_parse_end(parser.get()) != body.size() ||
        !json_object_is_type(value.get(), json_type_object)) {
        throw InvalidRequest{"body must be a single JSON object"};
    }
    return value;
}

json_object* field(const Json& obj, const char* name) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(obj.get(), name, &value) || !value) {
        throw InvalidRequest{std::string{"missing or null field: "} + name};
    }
    return value;
}

bool has(const Json& obj, const char* name) {
    return json_object_object_get_ex(obj.get(), name, nullptr);
}

std::string_view string_value(json_object* value) {
    return {json_object_get_string(value),
            static_cast<std::size_t>(json_object_get_string_len(value))};
}

template <class Int>
Int decimal(std::string_view value, const char* name) {
    Int integer{};
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, integer);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw InvalidRequest{std::string{"invalid decimal integer: "} + name};
    }
    return integer;
}

template <class Int = std::int64_t>
Int number(const Json& obj, const char* name) {
    auto* value = field(obj, name);
    if (json_object_is_type(value, json_type_string)) {
        return decimal<Int>(string_value(value), name);
    }
    if (json_object_is_type(value, json_type_int)) {
        constexpr std::int64_t safe = 9'007'199'254'740'991;
        const auto n = json_object_get_int64(value);
        if (n >= -safe && n <= safe) {
            return decimal<Int>(json_object_get_string(value), name);
        }
    }
    throw InvalidRequest{std::string{"use an integer or decimal string: "} +
                         name};
}

template <class Strong>
Strong id(const Json& obj, const char* name) {
    return Strong{number<decltype(std::declval<Strong>().value())>(obj, name)};
}

std::string_view string_field(const Json& obj, const char* name) {
    auto* value = field(obj, name);
    if (!json_object_is_type(value, json_type_string)) {
        throw InvalidRequest{std::string{"expected a string: "} + name};
    }
    return string_value(value);
}

OrderKey key(const Json& body) {
    return {id<SymbolId>(body, "symbol_id"), id<OrderId>(body, "order_id")};
}

Side side_field(const Json& body) {
    const auto side = string_field(body, "side");
    if (side == "bid") return Side::bid;
    if (side == "ask") return Side::ask;
    throw InvalidRequest{"side must be bid or ask"};
}

OrderConstraint constraint(const Json& body) {
    const auto type = string_field(body, "type");
    if (type == "budget") {
        if (has(body, "price") || has(body, "reserve_bid_price")) {
            throw InvalidRequest{
                "budget orders use budget_total instead of price fields"};
        }
        return BudgetLimit{NotionalSteps{number(body, "budget_total")}};
    }
    if (type != "gtc" && type != "ioc") {
        throw InvalidRequest{"type must be gtc, ioc or budget"};
    }
    if (has(body, "budget_total")) {
        throw InvalidRequest{"limit orders use price instead of budget_total"};
    }
    std::optional<Price> reserve;
    if (has(body, "reserve_bid_price")) {
        reserve.emplace(number(body, "reserve_bid_price"));
    }
    return LimitPrice{Price{number(body, "price")},
                      type == "gtc" ? LimitPolicy::gtc : LimitPolicy::ioc,
                      reserve};
}

Command make_order(const Json& body) {
    return PlaceOrder{
        key(body),
        id<UserId>(body, "user_id"),
        side_field(body),
        Quantity{number(body, "quantity")},
        constraint(body),
        has(body, "timestamp_ns") ? number(body, "timestamp_ns") : 0};
}

Command make_symbol(const Json& body) {
    return RegisterSymbol{
        {id<SymbolId>(body, "symbol_id"), id<CurrencyId>(body, "base"),
         id<CurrencyId>(body, "quote"), number(body, "base_scale"),
         number(body, "quote_scale"), number(body, "maker_fee_per_lot"),
         number(body, "taker_fee_per_lot")}};
}

using Decode = Command (*)(const Json&);
constexpr struct {
    std::string_view path;
    Decode decode;
} mutations[] = {
    {"/v1/orders", make_order},
    {"/v1/orders/cancel",
     [](const Json& b) -> Command {
         return CancelOrder{key(b), id<UserId>(b, "user_id")};
     }},
    {"/v1/orders/reduce",
     [](const Json& b) -> Command {
         return ReduceOrder{key(b), id<UserId>(b, "user_id"),
                            Quantity{number(b, "quantity")}};
     }},
    {"/v1/orders/move",
     [](const Json& b) -> Command {
         return MoveOrder{key(b), id<UserId>(b, "user_id"),
                          Price{number(b, "new_price")}};
     }},
    {"/v1/symbols", make_symbol},
    {"/v1/users",
     [](const Json& b) -> Command {
         return AddUser{id<UserId>(b, "user_id")};
     }},
    {"/v1/balances/adjust",
     [](const Json& b) -> Command {
         return AdjustBalance{
             id<UserId>(b, "user_id"),
             {id<CurrencyId>(b, "currency"), number(b, "units")},
             number<std::uint64_t>(b, "funding_id")};
     }},
};

const char* side_name(Side side) {
    return side == Side::bid ? "bid" : "ask";
}

void put_key(const Json& obj, OrderKey key) {
    put(obj, "symbol_id", integer(key.symbol.value()));
    put(obj, "order_id", integer(key.order.value()));
}

Json event_json(const Trade& trade) {
    auto obj = object();
    put(obj, "type", text("trade"));
    put(obj, "symbol_id", integer(trade.maker.symbol.value()));
    put(obj, "maker_order_id", integer(trade.maker.order.value()));
    put(obj, "maker_user_id", integer(trade.maker_user.value()));
    put(obj, "taker_order_id", integer(trade.taker.order.value()));
    put(obj, "taker_user_id", integer(trade.taker_user.value()));
    put(obj, "taker_side", text(side_name(trade.taker_side)));
    put(obj, "price", integer(trade.price.value()));
    put(obj, "quantity", integer(trade.quantity.value()));
    put(obj, "maker_filled", flag(trade.maker_filled));
    put(obj, "taker_filled", flag(trade.taker_filled));
    return obj;
}

Json event_json(const Reduce& reduce) {
    auto obj = object();
    put(obj, "type", text("reduce"));
    put_key(obj, reduce.key);
    put(obj, "user_id", integer(reduce.user.value()));
    put(obj, "side", text(side_name(reduce.side)));
    put(obj, "price", integer(reduce.price.value()));
    put(obj, "quantity", integer(reduce.quantity.value()));
    put(obj, "removed", flag(reduce.removed));
    return obj;
}

Json event_json(const RejectRemainder& remainder) {
    auto obj = object();
    put(obj, "type", text("reject_remainder"));
    put_key(obj, remainder.key);
    put(obj, "user_id", integer(remainder.user.value()));
    put(obj, "quantity", integer(remainder.quantity.value()));
    put(obj, "reason", text(reason_name(remainder.reason)));
    return obj;
}

Json money_json(Money money) {
    auto obj = object();
    put(obj, "currency", integer(money.currency.value()));
    put(obj, "units", integer(money.units));
    return obj;
}

Json report_json(const UserReport& user) {
    auto obj = object();
    put(obj, "user_id", integer(user.user.value()));
    put(obj, "suspended", flag(user.suspended));
    put(obj, "last_funding_id", integer(user.last_funding));
    auto available = array();
    for (const auto& amount : user.available) {
        append(available, money_json(amount));
    }
    put(obj, "available", std::move(available));

    auto frozen = array();
    for (const auto& hold : user.frozen) {
        auto item = money_json(hold.amount);
        put_key(item, hold.key);
        append(frozen, std::move(item));
    }
    put(obj, "frozen", std::move(frozen));

    auto orders = array();
    for (const auto& order : user.orders) {
        auto item = object();
        put_key(item, order.key);
        put(item, "side", text(side_name(order.side)));
        put(item, "price", integer(order.price.value()));
        put(item, "remaining", integer(order.remaining.value()));
        put(item, "priority_seq", integer(order.priority_seq.value()));
        put(item, "timestamp_ns", integer(order.timestamp_ns));
        if (order.reserve_bid_price) {
            put(item, "reserve_bid_price",
                integer(order.reserve_bid_price->value()));
        }
        append(orders, std::move(item));
    }
    put(obj, "orders", std::move(orders));
    return obj;
}

Json report_json(const TotalsReport& totals) {
    auto obj = object();
    auto currencies = array();
    for (const auto& total : totals.currencies) {
        auto row = object();
        put(row, "currency", integer(total.currency.value()));
        put(row, "available", integer(total.available));
        put(row, "frozen", integer(total.frozen));
        put(row, "fees", integer(total.fees));
        put(row, "net_deposits", integer(total.net_deposits));
        put(row, "conserved", flag(total.conserved()));
        append(currencies, std::move(row));
    }
    put(obj, "currencies", std::move(currencies));
    return obj;
}

Json levels_json(const std::vector<PriceLevel>& levels) {
    auto rows = array();
    for (const auto& level : levels) {
        auto row = object();
        put(row, "price", integer(level.price.value()));
        put(row, "quantity", integer(level.total_remaining.value()));
        put(row, "order_count", integer(level.order_count));
        append(rows, std::move(row));
    }
    return rows;
}

int http_status(Status status) {
    switch (status) {
        case Status::processed:
            return 200;
        case Status::invalid_input:
            return 400;
        case Status::unknown_symbol:
        case Status::unknown_order:
        case Status::unknown_user:
            return 404;
        case Status::user_suspended:
            return 403;
        case Status::duplicate_order:
        case Status::symbol_exists:
        case Status::user_exists:
        case Status::account_not_empty:
        case Status::duplicate_funding:
        case Status::stale_funding:
            return 409;
        case Status::insufficient_funds:
        case Status::price_over_reserve:
        case Status::settlement_failed:
            return 422;
        case Status::capacity_exhausted:
            return 503;
    }
    return 500;
}

Response result_json(const Result& result) {
    auto obj = object();
    put(obj, "status", text(status_name(result.outcome.status)));
    put(obj, "seq", integer(result.seq.value()));
    auto events = array();
    for (const auto& event : result.outcome.events) {
        append(events,
               std::visit([](const auto& info) { return event_json(info); },
                          event));
    }
    put(obj, "events", std::move(events));
    if (const auto* user = std::get_if<UserReport>(&result.report)) {
        put(obj, "report", report_json(*user));
    } else if (const auto* totals = std::get_if<TotalsReport>(&result.report)) {
        put(obj, "report", report_json(*totals));
    }
    return reply(http_status(result.outcome.status), obj);
}

bool is_json(std::string_view content_type) {
    auto type = content_type.substr(0, content_type.find(';'));
    const auto first = type.find_first_not_of(" \t");
    if (first == std::string_view::npos) return false;
    type = type.substr(first, type.find_last_not_of(" \t") - first + 1);
    constexpr std::string_view expected = "application/json";
    return std::ranges::equal(type, expected, [](char actual, char lower) {
        if (actual >= 'A' && actual <= 'Z') actual += 'a' - 'A';
        return actual == lower;
    });
}

Response wrong_method(const char* expected) {
    auto response = error(405, "method_not_allowed",
                          "unsupported method for this endpoint");
    response.allow = expected;
    return response;
}

}  // namespace

Response error(int status, const char* code, std::string_view detail) {
    auto obj = object();
    put(obj, "status", text(code));
    put(obj, "message", text(detail));
    return reply(status, obj);
}

Response route(Engine& engine, std::string_view method, std::string_view path,
               std::string_view content_type, std::string_view body) {
    try {
        if (path == "/health") {
            if (method != "GET") return wrong_method("GET");
            auto obj = object();
            put(obj, "status", text("ok"));
            return reply(200, obj);
        }
        for (const auto& endpoint : mutations) {
            if (path != endpoint.path) continue;
            if (method != "POST") return wrong_method("POST");
            if (!is_json(content_type)) {
                return error(415, "unsupported_media_type",
                             "expected application/json");
            }
            return result_json(
                engine.execute(endpoint.decode(parse_object(body))));
        }

        constexpr std::string_view users = "/v1/users/";
        constexpr std::string_view books = "/v1/books/";
        if (path.starts_with(users)) {
            if (method != "GET") return wrong_method("GET");
            const auto value =
                decimal<std::uint64_t>(path.substr(users.size()), "user_id");
            return result_json(engine.execute(QueryUser{UserId{value}}));
        }
        if (path.starts_with(books)) {
            if (method != "GET") return wrong_method("GET");
            const SymbolId symbol{
                decimal<std::uint32_t>(path.substr(books.size()), "symbol_id")};
            const auto snapshot = engine.book_snapshot(symbol, 20);
            if (!snapshot)
                return error(404, "unknown_symbol", "symbol has no order book");
            auto obj = object();
            put(obj, "status", text("processed"));
            put(obj, "seq", integer(engine.last_sequence().value()));
            put(obj, "symbol_id", integer(symbol.value()));
            put(obj, "asks", levels_json(snapshot->asks));
            put(obj, "bids", levels_json(snapshot->bids));
            return reply(200, obj);
        }
        if (path == "/v1/totals") {
            if (method != "GET") return wrong_method("GET");
            return result_json(engine.execute(QueryTotals{}));
        }
        return error(404, "not_found", "unknown endpoint");
    } catch (const InvalidRequest& ex) {
        return error(400, "invalid_request", ex.what());
    }
}

}  // namespace exchange::api
