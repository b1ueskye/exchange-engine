#ifndef EXCHANGE_ORDER_NODE_POOL_HPP
#define EXCHANGE_ORDER_NODE_POOL_HPP

#include "exchange/model.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace exchange {

struct NodeHandle {
    static constexpr auto none = std::numeric_limits<std::size_t>::max();
    std::size_t slot{none};
    std::uint64_t generation{};
    bool operator==(const NodeHandle&) const = default;
};

class PoolQueue;

class OrderNodePool {
public:
    struct Node {
        RestingOrder order;
        NodeHandle prev;
        NodeHandle next;
        const PoolQueue* owner{};
    };

    explicit OrderNodePool(std::size_t capacity,
                           std::uint64_t generation_limit =
                               std::numeric_limits<std::uint64_t>::max())
        : slots_(capacity),
          free_(capacity == 0 ? NodeHandle::none : 0),
          generation_limit_(generation_limit) {
        if (generation_limit == 0) {
            throw std::invalid_argument{"generation limit must be positive"};
        }
        for (std::size_t i = 0; i + 1 < slots_.size(); ++i) {
            slots_[i].free_next = i + 1;
        }
    }

    OrderNodePool(const OrderNodePool&) = delete;
    OrderNodePool& operator=(const OrderNodePool&) = delete;

    bool has_room() const {
        return free_ != NodeHandle::none;
    }

    std::optional<NodeHandle> acquire(const RestingOrder& order) {
        if (!has_room()) {
            return std::nullopt;
        }
        const auto pos = free_;
        auto& slot = slots_[pos];
        slot.node.emplace(Node{order, {}, {}, nullptr});
        free_ = slot.free_next;
        return NodeHandle{pos, slot.generation};
    }

    Node* get(NodeHandle handle) {
        if (handle.slot >= slots_.size()) {
            return nullptr;
        }
        auto& slot = slots_[handle.slot];
        return slot.generation == handle.generation && slot.node ? &*slot.node
                                                                 : nullptr;
    }

    bool release(NodeHandle handle) {
        auto* node = get(handle);
        if (!node || node->owner) {
            return false;
        }
        auto& slot = slots_[handle.slot];
        slot.node.reset();
        if (slot.generation < generation_limit_) {
            ++slot.generation;
            slot.free_next = free_;
            free_ = handle.slot;
        }
        return true;
    }

private:
    struct Slot {
        std::optional<Node> node;
        std::size_t free_next{NodeHandle::none};
        std::uint64_t generation{1};
    };

    std::vector<Slot> slots_;
    std::size_t free_;
    std::uint64_t generation_limit_;
};

class PoolQueue {
public:
    explicit PoolQueue(OrderNodePool& pool) : pool_(pool) {}
    PoolQueue(const PoolQueue&) = delete;
    PoolQueue& operator=(const PoolQueue&) = delete;

    ~PoolQueue() {
        while (!empty()) {
            erase(head_);
        }
    }

    bool empty() const {
        return size_ == 0;
    }
    std::size_t size() const {
        return size_;
    }
    NodeHandle head() const {
        return head_;
    }
    NodeHandle tail() const {
        return tail_;
    }

    OrderNodePool::Node* get(NodeHandle handle) const {
        auto* node = pool_.get(handle);
        return node && node->owner == this ? node : nullptr;
    }

    std::optional<NodeHandle> push_back(const RestingOrder& order) {
        const auto acquired = pool_.acquire(order);
        if (!acquired) {
            return std::nullopt;
        }
        auto* node = pool_.get(*acquired);
        node->owner = this;
        node->prev = tail_;
        if (auto* tail = get(tail_)) {
            tail->next = *acquired;
        } else {
            head_ = *acquired;
        }
        tail_ = *acquired;
        ++size_;
        return acquired;
    }

    bool erase(NodeHandle handle) {
        auto* node = get(handle);
        if (!node) {
            return false;
        }
        const auto prev = node->prev;
        const auto next = node->next;
        if (auto* left = get(prev)) {
            left->next = next;
        } else {
            head_ = next;
        }
        if (auto* right = get(next)) {
            right->prev = prev;
        } else {
            tail_ = prev;
        }
        --size_;
        node->owner = nullptr;
        return pool_.release(handle);
    }

private:
    OrderNodePool& pool_;
    NodeHandle head_;
    NodeHandle tail_;
    std::size_t size_{};
};

}  // namespace exchange

#endif
