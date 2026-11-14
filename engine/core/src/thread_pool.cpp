#include <astra/core/thread_pool.hpp>

#include <algorithm>

namespace astra::core {

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0) {
        worker_count = std::max(1u, std::thread::hardware_concurrency());
    }

    workers_.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        {
            const std::lock_guard lock{mutex_};
            stopping_ = true;
            shutdown_mode_ = ShutdownMode::cancel_pending;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown(ShutdownMode mode) {
    const auto current_thread = std::this_thread::get_id();
    if (std::any_of(workers_.begin(), workers_.end(), [current_thread](const auto& worker) {
            return worker.get_id() == current_thread;
        })) {
        throw std::logic_error("ThreadPool::shutdown cannot be called from a worker");
    }

    const std::lock_guard shutdown_lock{shutdown_mutex_};

    {
        const std::lock_guard lock{mutex_};
        if (!stopping_) {
            stopping_ = true;
            shutdown_mode_ = mode;
            if (mode == ShutdownMode::cancel_pending) {
                std::queue<std::function<void()>> empty;
                tasks_.swap(empty);
            }
        }
    }

    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::size_t ThreadPool::worker_count() const noexcept {
    return workers_.size();
}

bool ThreadPool::is_stopping() const noexcept {
    const std::lock_guard lock{mutex_};
    return stopping_;
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

            if (stopping_ && (shutdown_mode_ == ShutdownMode::cancel_pending || tasks_.empty())) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // packaged_task stores user exceptions in its future, keeping workers alive.
        task();
    }
}

} // namespace astra::core
