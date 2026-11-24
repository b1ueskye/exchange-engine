#ifndef EXCHANGE_ASYNC_ENGINE_HPP
#define EXCHANGE_ASYNC_ENGINE_HPP

#include "exchange/bounded_queue.hpp"
#include "exchange/engine.hpp"

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

namespace exchange {

enum class SubmitError { full, closed };
using Submission = std::variant<std::future<Result>, SubmitError>;

class AsyncEngine {
public:
    using Processor = std::function<Result(const Command&)>;

    explicit AsyncEngine(std::size_t capacity, BookKind kind = BookKind::list,
                         std::size_t pool_capacity = 4096)
        : AsyncEngine(capacity, [engine = std::make_shared<Engine>(kind, pool_capacity)]
                                (const Command& command) { return engine->execute(command); }) {}

    AsyncEngine(std::size_t capacity, Processor processor)
        : processor_(std::move(processor)), queue_(capacity),
          worker_([this] { consume(); }) {}

    ~AsyncEngine() { close(); }

    Submission submit(Command command) {
        Work item{std::move(command), {}};
        auto answer = item.promise.get_future();
        const auto status = queue_.try_push(std::move(item));
        if (status == PushStatus::accepted) {
            return answer;
        }
        return status == PushStatus::full ? SubmitError::full : SubmitError::closed;
    }

    void close() {
        queue_.close();
        const std::lock_guard lock(join_mutex_);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    struct Work {
        Command command;
        std::promise<Result> promise;
    };

    void consume() {
        while (auto item = queue_.pop()) {
            try {
                item->promise.set_value(processor_(item->command));
            } catch (...) {
                item->promise.set_exception(std::current_exception());
            }
        }
    }

    Processor processor_;
    BoundedQueue<Work> queue_;
    std::mutex join_mutex_;
    std::jthread worker_;
};

}

#endif
