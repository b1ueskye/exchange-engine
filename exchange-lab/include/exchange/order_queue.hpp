#ifndef EXCHANGE_ORDER_QUEUE_HPP
#define EXCHANGE_ORDER_QUEUE_HPP

#include "exchange/order_node_pool.hpp"

#include <iterator>
#include <list>
#include <variant>

namespace exchange {

class OrderQueue {
    using List = std::list<RestingOrder>;

public:
    class Handle {
    public:
        RestingOrder& operator*() const {
            if (const auto* iter = std::get_if<List::iterator>(&value_)) {
                return **iter;
            }
            auto* node = std::get<PoolQueue>(owner_->data_)
                             .get(std::get<NodeHandle>(value_));
            if (!node) {
                throw std::logic_error{"stale order handle"};
            }
            return node->order;
        }
        RestingOrder* operator->() const {
            return &**this;
        }

    private:
        friend class OrderQueue;
        using Position = std::variant<List::iterator, NodeHandle>;
        Handle(OrderQueue* owner, Position value)
            : owner_(owner), value_(value) {}
        OrderQueue* owner_;
        Position value_;
    };

    class Cursor {
    public:
        const RestingOrder& operator*() const {
            if (const auto* iter = std::get_if<List::const_iterator>(&value_)) {
                return **iter;
            }
            return pool_node().order;
        }

        Cursor& operator++() {
            if (auto* iter = std::get_if<List::const_iterator>(&value_)) {
                ++*iter;
            } else {
                value_ = pool_node().next;
            }
            return *this;
        }

        bool operator==(const Cursor&) const = default;

    private:
        friend class OrderQueue;
        using Position = std::variant<List::const_iterator, NodeHandle>;
        Cursor(const OrderQueue* owner, Position value)
            : owner_(owner), value_(value) {}

        const OrderNodePool::Node& pool_node() const {
            const auto* node = std::get<PoolQueue>(owner_->data_)
                                   .get(std::get<NodeHandle>(value_));
            if (!node) {
                throw std::logic_error{"stale queue cursor"};
            }
            return *node;
        }

        const OrderQueue* owner_;
        Position value_;
    };

    explicit OrderQueue(OrderNodePool* pool) {
        if (pool) {
            data_.emplace<PoolQueue>(*pool);
        }
    }

    std::size_t size() const {
        if (const auto* list = std::get_if<List>(&data_)) {
            return list->size();
        }
        return std::get<PoolQueue>(data_).size();
    }
    bool empty() const {
        return size() == 0;
    }

    const RestingOrder& back() const {
        if (const auto* list = std::get_if<List>(&data_)) {
            return list->back();
        }
        const auto& queue = std::get<PoolQueue>(data_);
        return queue.get(queue.tail())->order;
    }

    bool push_back(const RestingOrder& order) {
        if (auto* list = std::get_if<List>(&data_)) {
            list->push_back(order);
            return true;
        }
        return std::get<PoolQueue>(data_).push_back(order).has_value();
    }

    Handle last() {
        if (auto* list = std::get_if<List>(&data_)) {
            return {this, std::prev(list->end())};
        }
        return {this, std::get<PoolQueue>(data_).tail()};
    }

    void erase(Handle handle) {
        if (handle.owner_ != this) {
            throw std::logic_error{"wrong order queue"};
        }
        if (auto* list = std::get_if<List>(&data_)) {
            list->erase(std::get<List::iterator>(handle.value_));
        } else if (!std::get<PoolQueue>(data_).erase(
                       std::get<NodeHandle>(handle.value_))) {
            throw std::logic_error{"could not erase order handle"};
        }
    }

    void pop_back() {
        erase(last());
    }

    Cursor begin() const {
        if (const auto* list = std::get_if<List>(&data_)) {
            return {this, list->begin()};
        }
        return {this, std::get<PoolQueue>(data_).head()};
    }

    Cursor end() const {
        if (const auto* list = std::get_if<List>(&data_)) {
            return {this, list->end()};
        }
        return {this, NodeHandle{}};
    }

private:
    std::variant<List, PoolQueue> data_;
};

}  // namespace exchange

#endif
