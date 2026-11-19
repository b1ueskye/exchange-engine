#include<iostream>
#include <iterator>
#include <vector>
#include <list>
#include <map>
#include<unordered_map>

struct AskOrder
{
    /* data */
    bool operator()(int a, int b) const
    {
        return a < b;
    }
};

struct BidOrder
{
    /* data */
    bool operator()(int a, int b) const
    {
        return a > b;
    }
};

int main() {
    std::vector<int> prices{101, 103, 100, 101, 100};
    const auto initial_capacity = prices.capacity();
    prices.reserve(initial_capacity + 1);
    if (prices.capacity() <= initial_capacity || prices.at(2) != 100) {
        std::cerr << "FAIL: vector reserve\n";
        return 1;
    }

    std::map<int, int, AskOrder> asks;
    std::map<int, int, BidOrder> bids;
    for (const int price : prices) {
        asks.try_emplace(price, 0);
        bids.try_emplace(price, 0);
        ++asks.at(price);
        ++bids.at(price);
    }

    std::vector<int> ask_prices;
    std::vector<int> bid_prices;
    for (const auto& [price, lots] : asks) {
        ask_prices.push_back(price);
        if (lots <= 0) {
            return 1;
        }
    }
    for (const auto& [price, lots] : bids) {
        bid_prices.push_back(price);
        if (lots <= 0) {
            return 1;
        }
    }
    if (ask_prices != std::vector<int>{100, 101, 103} ||
        bid_prices != std::vector<int>{103, 101, 100} ||
        asks.at(101) != 2 || asks.at(100) != 2 || bids.at(103) != 1) {
        std::cerr << "FAIL: price ordering or aggregation\n";
        return 1;
    }

    std::list<int> fifo{7, 8, 9};
    const auto first = fifo.begin();
    const auto last = std::next(first, 2);
    fifo.erase(std::next(first));
    if (fifo.size() != 2 || *first != 7 || *last != 9) {
        std::cerr << "FAIL: surviving list handles\n";
        return 1;
    }

    std::unordered_map<int, int> by_id{{7, 101}};
    const int* price_view = &by_id.at(7);
    const auto before = by_id.bucket_count();
    by_id.rehash(before + 1);
    if (by_id.bucket_count() <= before || &by_id.at(7) != price_view ||
        *price_view != 101 || by_id.find(8) != by_id.end()) {
        std::cerr << "FAIL: hash lookup or surviving element\n";
        return 1;
    }

    const auto bad = [](int a, int b) { return a <= b; };
    if (!bad(101, 101) || AskOrder{}(101, 101) || BidOrder{}(101, 101)) {
        std::cerr << "FAIL: comparator contract\n";
        return 1;
    }
    std::cout << "asks 100,101,103; bids 103,101,100; fifo 7,9\n";
    return 0;
}