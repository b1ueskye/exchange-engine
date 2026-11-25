#ifndef EXCHANGE_TRADING_INTEGRATION_FIXTURES_HPP
#define EXCHANGE_TRADING_INTEGRATION_FIXTURES_HPP
#include "exchange/engine.hpp"
namespace integration_fixture {
using namespace exchange;
inline SymbolSpec spec(bool futures) {
    return {SymbolId{1},CurrencyId{10},CurrencyId{20},1,1,1,2,
            futures ? SymbolType::futures : SymbolType::spot,100,120};
}
inline PlaceOrder limit(std::uint64_t id,std::uint64_t user,Side side,std::int64_t q,std::int64_t p,
                        LimitPolicy policy=LimitPolicy::gtc) {
    return {{SymbolId{1},OrderId{id}},UserId{user},side,Quantity{q},
            LimitPrice{Price{p},policy,side==Side::bid?std::optional{Price{p+50}}:std::nullopt},0};
}
inline std::vector<Command> history(bool futures,std::uint32_t seed) {
    const std::int64_t p=98+seed%5;
    auto s=spec(futures);
    std::vector<Command> out{RegisterSymbol{s},AddUsers{{UserId{1},UserId{2},UserId{3}}},
        AdjustBalance{UserId{1},{s.quote,10000},1},AdjustBalance{UserId{2},{s.quote,10000},1},
        AdjustBalance{UserId{3},{s.quote,1},1}};
    if (!futures) { out.push_back(AdjustBalance{UserId{2},{s.base,10},2}); out.push_back(AdjustBalance{UserId{1},{s.base,10},2}); }
    const std::vector<Command> tail{limit(1,2,Side::ask,5,p),limit(2,1,Side::bid,5,p),
        QueryUser{UserId{1}},QueryFingerprint{},limit(3,2,Side::bid,2,p+10),
        limit(4,1,Side::ask,2,p+10,LimitPolicy::ioc),limit(5,1,Side::ask,3,p+20),
        ReduceOrder{{s.symbol,OrderId{5}},UserId{1},Quantity{1}},
        MoveOrder{{s.symbol,OrderId{5}},UserId{1},Price{p+21}},
        CancelOrder{{s.symbol,OrderId{5}},UserId{1}},limit(6,3,Side::bid,1,p),
        AdjustBalance{UserId{1},{s.quote,1},1},limit(7,2,Side::bid,3,p+30),
        limit(8,1,Side::ask,3,p+30,LimitPolicy::ioc),QueryTotals{},AdjustBalance{UserId{1},{s.quote,1},1}};
    out.insert(out.end(),tail.begin(),tail.end());return out;
}
}
#endif
