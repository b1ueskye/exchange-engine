#include "exchange/async_engine.hpp"
#include "exchange/persistence.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

namespace {
using namespace exchange;
namespace fs = std::filesystem;

const SymbolSpec spec{SymbolId{1}, CurrencyId{10}, CurrencyId{20}, 10, 2, 1, 3};
const UserId buyer{1};
const UserId seller{2};

void require(bool ok, const char* label) {
    if (!ok) {
        throw std::logic_error{label};
    }
}

PlaceOrder ask(OrderId id, std::int64_t timestamp) {
    return {{spec.symbol, id}, seller, Side::ask, Quantity{1},
            LimitPrice{Price{100}, LimitPolicy::gtc, {}}, timestamp};
}

PlaceOrder bid() {
    return {{spec.symbol, OrderId{9}}, buyer, Side::bid, Quantity{2},
            LimitPrice{Price{105}, LimitPolicy::ioc, {}}, 0};
}

Bytes read_file(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"missing recovery file"};
    }
    return read_binary(input);
}

void produce(const fs::path& dir, std::string_view mode) {
    fs::create_directories(dir);
    std::ofstream log{dir / "journal.bin", std::ios::binary | std::ios::trunc};
    auto append = [&](std::span<const std::byte> bytes) { write_binary(log, bytes); };
    auto live = std::make_shared<JournaledEngine>(append);
    const auto submit = [&](const Command& command) {
        require(live->execute(command).outcome.status == Status::processed, "setup rejected");
    };
    submit(RegisterSymbol{spec});
    submit(AddUsers{{buyer, seller}});
    submit(AdjustBalance{buyer, {spec.quote, 1000}, 1});
    submit(AdjustBalance{seller, {spec.base, 50}, 1});
    submit(ask(OrderId{7}, 99));
    SnapshotStore::publish(dir / "snapshot.bin", live->state());

    if (mode == "async_close") {
        AsyncEngine async{2, [live](const Command& cmd) { return live->execute(cmd); }};
        auto accepted = async.submit(ask(OrderId{8}, 98));
        async.close();
        require(std::get<std::future<Result>>(accepted).get().seq == Seq{6}, "close did not drain");
        return;
    }

    const auto frame = encode_record(Seq{6}, ask(OrderId{8}, 98));
    if (mode == "torn") {
        append(std::span<const std::byte>{frame}.first(frame.size() - 3));
    } else if (mode == "ack_lost") {
        append(frame);
    } else if (mode == "bad") {
        auto corrupted = frame;
        corrupted.back() ^= std::byte{1};
        append(corrupted);
    } else {
        submit(ask(OrderId{8}, 98));
        if (mode == "before_publish") {
            std::ofstream temp{dir / "snapshot.bin.tmp", std::ios::binary | std::ios::trunc};
            write_binary(temp, SnapshotStore::encode(live->state()));
            temp.close();
            require(static_cast<bool>(temp), "could not close staged snapshot");
        } else if (mode == "after_publish") {
            SnapshotStore::publish(dir / "snapshot.bin", live->state());
        } else {
            throw std::invalid_argument{"unknown failure mode"};
        }
    }
    std::_Exit(42);
}

void check_restart(const fs::path& dir, std::string_view mode) {
    auto restored = SnapshotStore::decode(read_file(dir / "snapshot.bin"));
    require(restored->last_sequence() == Seq{mode == "after_publish" ? 6u : 5u},
            "wrong checkpoint");
    const auto journal = scan_log(read_file(dir / "journal.bin"));
    const auto replayed = replay_log(*restored, journal);
    const bool torn = mode == "torn";
    require(journal.truncated_tail == torn, "tail classification");
    const auto last = Seq{torn ? 5u : 6u};
    require(restored->last_sequence() == last, "wrong recovery cursor");
    require(replayed.size() == (torn || mode == "after_publish" ? 0u : 1u), "wrong replay suffix");
    const auto before = restored->book_orders(spec.symbol);
    require(before && before->size() == (torn ? 1u : 2u), "wrong resting count");
    require(before->front().key.order == OrderId{7}, "first maker changed");
    if (!torn) {
        require(before->at(1).key.order == OrderId{8}, "second maker changed");
    }

    if (journal.truncated_tail) {
        fs::resize_file(dir / "journal.bin", journal.valid_bytes);
    }
    std::ofstream out{dir / "journal.bin", std::ios::binary | std::ios::app};
    JournaledEngine resumed{[&](auto bytes) { write_binary(out, bytes); }, std::move(restored)};
    const auto result = resumed.execute(bid());
    require(result.seq == Seq{last.value() + 1}, "sequence did not continue");
    require(result.outcome.events.size() == 2, "wrong trade or reject count");
    require(std::get<Trade>(result.outcome.events.at(0)).maker.order == OrderId{7},
            "wrong recovered FIFO");
    if (torn) {
        require(std::get<RejectRemainder>(result.outcome.events.at(1)).quantity == Quantity{1},
                "missing IOC tail");
    } else {
        require(std::get<Trade>(result.outcome.events.at(1)).maker.order == OrderId{8},
                "second maker missing");
    }
    const auto duplicate = resumed.execute(AdjustBalance{buyer, {spec.quote, 1000}, 1});
    require(duplicate.outcome.status == Status::duplicate_funding, "funding applied twice");
    require(resumed.state().ledger().available(buyer, spec.base) ==
            Money{spec.base, torn ? 10 : 20}, "wrong buyer asset");
    require(resumed.state().ledger().available(buyer, spec.quote) ==
            Money{spec.quote, torn ? 797 : 594}, "wrong buyer cash");
    require(!resumed.state().ledger().held({spec.symbol, OrderId{7}}), "hold leaked");

    const auto totals = resumed.execute(QueryTotals{});
    for (const auto& row : std::get<TotalsReport>(totals.report).currencies) {
        require(row.conserved(), "currency did not conserve");
    }
    out.close();
    require(static_cast<bool>(out), "could not close resumed journal");
    const auto appended = scan_log(read_file(dir / "journal.bin"));
    require(!appended.truncated_tail && appended.records.back().seq == duplicate.seq,
            "appended log unreadable");
    auto second = SnapshotStore::decode(read_file(dir / "snapshot.bin"));
    const auto again = replay_log(*second, appended);
    require(again.size() >= 2 && again[again.size() - 2].outcome == result.outcome,
            "second replay changed trade events");
    second->execute_at(QueryTotals{}, totals.seq);
    require(SnapshotStore::encode(*second) == SnapshotStore::encode(resumed.state()),
            "second replay changed canonical state");
    std::cout << "run_id=restart seq=" << totals.seq.value() << '\n';
}

}

int main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }
    const std::string_view action{argv[1]};
    const fs::path dir{argv[2]};
    const std::string_view mode{argv[3]};
    try {
        if (action == "produce") {
            produce(dir, mode);
        } else if (action == "restart") {
            check_restart(dir, mode);
        } else {
            return 2;
        }
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 3;
    }
}
