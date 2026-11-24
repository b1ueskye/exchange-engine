#include "exchange/order_node_pool.hpp"
#include "test_checks.hpp"
#include "trading_fixtures.hpp"

int main() {
    using namespace exchange;
    using trading_fixture::rest;
    TestChecks checks;
    OrderNodePool pool{3};
    PoolQueue fifo{pool};
    PoolQueue other{pool};
    const auto a = fifo.push_back(rest(1, Side::ask, 100, 1, 1, 0));
    const auto b = fifo.push_back(rest(2, Side::ask, 100, 1, 2, 0));
    const auto c = fifo.push_back(rest(3, Side::ask, 100, 1, 3, 0));
    if (!a || !b || !c) {
        return 1;
    }
    checks.require(!fifo.push_back(rest(4, Side::ask, 100, 1, 4, 0)) &&
                   fifo.head() == *a && fifo.tail() == *c, "full pool keeps queue");
    checks.require(!other.erase(*b) && !pool.release(*b), "owner check");
    checks.require(fifo.erase(*b) && !fifo.erase(*b) && !pool.get(*b) &&
                   fifo.get(*a)->next == *c && fifo.get(*c)->prev == *a, "unlink middle");
    const auto d = other.push_back(rest(4, Side::ask, 101, 1, 4, 0));
    checks.require(d && d->slot == b->slot && d->generation != b->generation &&
                   !pool.get(*b), "recycled slot rejects stale handle");
    checks.require(fifo.erase(*a) && fifo.head() == *c &&
                   fifo.erase(*c) && fifo.empty() && fifo.tail() == NodeHandle{}, "erase head and tail");

    OrderNodePool tiny{1, 2};
    const auto sample = rest(5, Side::ask, 100, 1, 1, 0);
    const auto first = tiny.acquire(sample);
    checks.require(first && tiny.release(*first), "first release");
    const auto last = tiny.acquire(sample);
    checks.require(last && last->generation == 2 && tiny.release(*last) &&
                   !tiny.acquire(sample) && !tiny.get(*last), "generation retires slot");
    return checks.exit_code();
}
