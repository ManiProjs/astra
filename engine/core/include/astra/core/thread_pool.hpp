#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace astra::core {

enum class ShutdownMode {
    drain,
    cancel_pending,
};

// A fixed-size executor. Destruction drains accepted work and joins every worker.
class ThreadPool final {
  public:
    // A worker_count of zero selects hardware_concurrency(), with a minimum of one.
    explicit ThreadPool(std::size_t worker_count = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <typename Function, typename... Args>
    [[nodiscard]] auto submit(Function&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>;

        auto task = std::make_shared<std::packaged_task<Result()>>(
            [callable = std::decay_t<Function>(std::forward<Function>(function)),
             arguments = std::tuple<std::decay_t<Args>...>(
                 std::forward<Args>(args)...)]() mutable -> Result {
                return std::apply(
                    [&callable](auto&&... unpacked) -> Result {
                        return std::invoke(std::move(callable),
                                           std::forward<decltype(unpacked)>(unpacked)...);
                    },
                    std::move(arguments));
            });
        auto result = task->get_future();

        {
            const std::lock_guard lock{mutex_};
            if (stopping_) {
                throw std::runtime_error("cannot submit work to a stopped ThreadPool");
            }
            tasks_.emplace([task] { (*task)(); });
        }
        condition_.notify_one();
        return result;
    }

    // Drain runs all accepted tasks. cancel_pending discards queued tasks; running
    // tasks are always allowed to finish. Calling shutdown from one of this pool's
    // workers throws std::logic_error to avoid attempting to join the current thread.
    void shutdown(ShutdownMode mode = ShutdownMode::drain);

    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;

  private:
    void worker_loop();

    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
    ShutdownMode shutdown_mode_{ShutdownMode::drain};
};

} // namespace astra::core
