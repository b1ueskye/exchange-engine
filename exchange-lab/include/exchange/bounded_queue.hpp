#ifndef EXCHANGE_BOUNDED_QUEUE_HPP
#define EXCHANGE_BOUNDED_QUEUE_HPP

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace exchange {

enum class PushStatus { accepted, full, closed };

template<class T>
class BoundedQueue {
    static_assert(std::is_nothrow_move_constructible_v<T>);
public:
    explicit BoundedQueue(std::size_t capacity) : slots_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument{"queue capacity must be positive"};
        }
    }

    PushStatus try_push(T&& item) {
        {
            const std::lock_guard lock(mutex_);
            if (closed_) {
                return PushStatus::closed;
            }
            if (size_ == slots_.size()) {
                return PushStatus::full;
            }
            slots_[tail_].emplace(std::move(item));
            tail_ = next(tail_);
            ++size_;
        }
        ready_.notify_one();
        return PushStatus::accepted;
    }

    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || size_ != 0; });
        if (size_ == 0) {
            return std::nullopt;
        }
        auto result = std::move(slots_[head_]);
        slots_[head_].reset();
        head_ = next(head_);
        --size_;
        return result;
    }

    void close() {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

private:
    std::size_t next(std::size_t pos) const {
        return pos + 1 == slots_.size() ? 0 : pos + 1;
    }

    std::vector<std::optional<T>> slots_;
    std::size_t head_{};
    std::size_t tail_{};
    std::size_t size_{};
    bool closed_{};
    std::mutex mutex_;
    std::condition_variable ready_;
};

}

#endif
