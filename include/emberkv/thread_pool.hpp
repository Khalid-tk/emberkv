#pragma once

// A fixed-size thread pool. submit() returns a std::future, so results and
// exceptions both travel back to the caller. Used by BGSAVE in the server and
// by the concurrency tests and benchmark.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace emberkv {

/// A move-only, type-erased `void()` callable. std::function can't hold a
/// std::packaged_task (it requires copyable targets) and std::move_only_function
/// is C++23, so this is the classic concept/model pattern in a dozen lines.
class Task {
public:
    Task() = default;

    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, Task> && std::invocable<F&>)
    Task(F&& f) : self_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}  // NOLINT: implicit by design

    void operator()() { self_->run(); }
    explicit operator bool() const noexcept { return self_ != nullptr; }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void run() = 0;
    };
    template <class F>
    struct Model final : Concept {
        explicit Model(F&& f) : fn(std::move(f)) {}
        explicit Model(const F& f) : fn(f) {}
        void run() override { fn(); }
        F fn;
    };
    std::unique_ptr<Concept> self_;
};

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads = std::max(1u, std::thread::hardware_concurrency())) {
        if (threads == 0) throw std::invalid_argument("ThreadPool needs at least one thread");
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this](std::stop_token stop) { work(stop); });
    }

    /// Finishes every task already queued, then joins the workers.
    ~ThreadPool() {
        for (auto& w : workers_) w.request_stop();
        cv_.notify_all();
        // std::jthread joins in its destructor.
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F, class... Args>
        requires std::invocable<F, Args...>
    [[nodiscard]] auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        // C++20 pack init-capture: arguments are moved into the task, not copied.
        std::packaged_task<R()> task(
            [fn = std::forward<F>(f), ... captured = std::forward<Args>(args)]() mutable -> R {
                return std::invoke(std::move(fn), std::move(captured)...);
            });
        std::future<R> result = task.get_future();
        {
            std::scoped_lock lock(mutex_);
            queue_.emplace_back(std::move(task));
        }
        cv_.notify_one();
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

private:
    void work(std::stop_token stop) {
        while (true) {
            Task task;
            {
                std::unique_lock lock(mutex_);
                // Returns false only when stop was requested *and* the queue is empty.
                if (!cv_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();  // packaged_task stores any exception in the future
        }
    }

    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Task> queue_;
    std::vector<std::jthread> workers_;  // last: joined before the queue is destroyed
};

}  // namespace emberkv
