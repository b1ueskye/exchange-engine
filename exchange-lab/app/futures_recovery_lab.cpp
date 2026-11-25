#include "exchange/persistence.hpp"
#include "trading_integration_fixtures.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace {
using namespace exchange;
Bytes load(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return read_binary(input);
}
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error{"futures recovery check"};
    }
}
void write(const std::filesystem::path& dir, bool torn) {
    std::filesystem::create_directories(dir);
    std::ofstream log{dir / "commands.bin", std::ios::binary | std::ios::trunc};
    JournaledEngine e{
        [&](std::span<const std::byte> b) { write_binary(log, b); },
        std::make_unique<Engine>(BookKind::pooled, 64)};
    const auto commands = integration_fixture::history(true, 7);
    const std::size_t committed = torn ? 17 : commands.size();
    for (std::size_t i = 0; i < committed; ++i) {
        e.execute(commands[i]);
        if (i == 11) {
            SnapshotStore::publish(dir / "state.bin", e.state());
        }
    }
    if (torn) {
        const auto frame =
            encode_record(e.state().next_sequence(), commands.at(committed));
        write_binary(log,
                     std::span<const std::byte>{frame}.first(frame.size() / 2));
    }
    std::_Exit(42);
}

void recover(const std::filesystem::path& dir, bool torn) {
    const auto scanned = scan_log(load(dir / "commands.bin"));
    require(scanned.truncated_tail == torn);
    auto e = SnapshotStore::decode(load(dir / "state.bin"));
    const auto results = replay_log(*e, scanned);
    const auto commands = integration_fixture::history(true, 7);
    Engine expected{BookKind::pooled, 64};
    const std::size_t committed = torn ? 17 : commands.size();
    std::vector<Result> receipts;
    for (std::size_t i = 0; i < committed; ++i) {
        receipts.push_back(expected.execute(commands[i]));
    }
    for (const auto& r : results) {
        require(r.outcome == receipts.at(r.seq.value() - 1).outcome);
    }
    require(SnapshotStore::encode(*e) == SnapshotStore::encode(expected));
    std::filesystem::resize_file(dir / "commands.bin", scanned.valid_bytes);
    {
        std::ofstream log{dir / "commands.bin",
                          std::ios::binary | std::ios::app};
        JournaledEngine resumed{
            [&](std::span<const std::byte> b) { write_binary(log, b); },
            std::move(e)};
        for (std::size_t i = committed; i < commands.size(); ++i) {
            require(resumed.execute(commands[i]).outcome ==
                    expected.execute(commands[i]).outcome);
        }
        require(SnapshotStore::encode(resumed.state()) ==
                SnapshotStore::encode(expected));
        SnapshotStore::publish(dir / "state.bin", resumed.state());
    }
    auto again = SnapshotStore::decode(load(dir / "state.bin"));
    require(replay_log(*again, scan_log(load(dir / "commands.bin"))).empty());
    require(SnapshotStore::encode(*again) == SnapshotStore::encode(expected));
    const auto duplicate =
        again->execute(AdjustBalance{UserId{1}, {CurrencyId{20}, 1}, 1});
    require(duplicate.outcome.status == Status::duplicate_funding &&
            std::get<TotalsReport>(again->execute(QueryTotals{}).report)
                .currencies.front()
                .conserved());
    std::cout << "futures recovered; torn=" << torn
              << "; cash=10100; fees=15\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            throw std::invalid_argument{
                "write|recover directory complete|torn"};
        }
        const std::string action{argv[1]}, mode{argv[3]};
        if (mode != "complete" && mode != "torn") {
            throw std::invalid_argument{"mode"};
        }
        if (action == "write") {
            write(argv[2], mode == "torn");
        } else if (action == "recover") {
            recover(argv[2], mode == "torn");
        } else {
            throw std::invalid_argument{"action"};
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
