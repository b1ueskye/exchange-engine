#include "exchange/persistence.hpp"
#include "trading_integration_fixtures.hpp"
#include "test_checks.hpp"

namespace exchange {
struct MarginTestAccess {
    static void damage_pending(Engine& e) {
        auto& ledger=const_cast<Ledger&>(e.ledger());
        ++ledger.accounts_.at(UserId{1}).positions.at(SymbolId{1}).pending_buy;
    }
};
}
int main() {
    using namespace integration_fixture;
    TestChecks checks;
    for (const auto kind : {BookKind::list, BookKind::pooled}) {
        Bytes journal;
        JournaledEngine e{[&](std::span<const std::byte> bytes) {
            journal.insert(journal.end(), bytes.begin(), bytes.end());
        }, std::make_unique<Engine>(kind, 64)};
        const auto script = history(true,7);
        for (std::size_t i = 0; i < 12; ++i) { e.execute(script[i]); }
        const auto snap = SnapshotStore::encode(e.state());
        for (std::size_t i = 12; i < script.size(); ++i) { e.execute(script[i]); }
        const auto expected = SnapshotStore::encode(e.state());
        auto recovered = SnapshotStore::decode(snap);
        const auto results = replay_log(*recovered, scan_log(journal));
        checks.require(!results.empty() && SnapshotStore::encode(*recovered) == expected,
                       "position pending and funding survive suffix replay");
        auto only_log = std::make_unique<Engine>(kind, 64);
        replay_log(*only_log, scan_log(journal));
        checks.require(SnapshotStore::encode(*only_log) == expected, "log-only recovery agrees");
        const auto refuse = [&](Bytes damaged, const char* message) {
            bool caught = false;
            try { SnapshotStore::decode(damaged); } catch (const std::exception&) { caught = true; }
            checks.require(caught, message);
        };
        auto damaged = snap; damaged.pop_back(); refuse(damaged, "truncation refused");
        damaged = snap; damaged.at(20) ^= std::byte{1}; refuse(damaged, "checksum refused");
        damaged = snap; damaged.at(4) = std::byte{9}; refuse(damaged, "unknown version refused");
        auto invalid=SnapshotStore::decode(snap);
        MarginTestAccess::damage_pending(*invalid);
        refuse(SnapshotStore::encode(*invalid), "valid checksum cannot hide pending mismatch");

    }
    JournaledEngine failing{[](std::span<const std::byte>) { throw std::runtime_error{"disk"}; },
                          std::make_unique<Engine>()};
    for (int i = 0; i < 2; ++i) {
        bool refused = false;
        try { failing.execute(AddUser{UserId{1}}); } catch (const std::exception&) { refused = true; }
        checks.require(refused && failing.state().last_sequence() == Seq{0}, "append failure locks before execution");
    }
    wire::Writer payload;
    payload.u(0);payload.u(0,1);payload.u(64);
    for(int i=0;i<5;++i) payload.u(0,4); // markets,accounts,holds,fees,net
    wire::Writer old;old.u(0x31504e53,4);old.u(1,1);old.u(payload.data.size(),4);
    old.data.insert(old.data.end(),payload.data.begin(),payload.data.end());old.u(wire::checksum(old.data),4);
    checks.require(SnapshotStore::decode(old.data)->last_sequence()==Seq{0},"v1 snapshot migrates to spot defaults");
    Engine debt;auto zero=spec(true);zero.margin_buy=zero.margin_sell=0;
    debt.execute(RegisterSymbol{zero});debt.execute(AddUsers{{UserId{1},UserId{2}}});
    debt.execute(limit(1,2,Side::ask,1,100));debt.execute(limit(2,1,Side::bid,1,100));
    checks.require(debt.ledger().available(UserId{1},zero.quote)->units==-2 &&
        SnapshotStore::encode(*SnapshotStore::decode(SnapshotStore::encode(debt)))==SnapshotStore::encode(debt),
        "opening fee may produce signed cash and survives restore");
    return checks.exit_code();
}
