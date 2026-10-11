#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace dingosdk::server {
// A few threads that share one batch of jobs with the thread that asks, which waits for the
// last of them. For the parts of the server's pass that are the same work once per player and
// touch nothing of each other's.
class WorkerPool {
  public:
    // `threads`: how many besides the caller's own; 0 runs everything on the caller's.
    explicit WorkerPool(unsigned threads) {
        try {
            for (unsigned index = 0; index < threads; ++index) threads_.emplace_back([this] { work(); });
        } catch (const std::system_error &) {} // fewer than asked for, or none
    }
    ~WorkerPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
    }
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;
    std::size_t threads() const noexcept { return threads_.size(); }
    // job(0) ... job(count - 1), each once, on these threads and the caller's; back when all are
    // done. A job must not throw.
    void run(std::size_t count, const std::function<void(std::size_t)> &job) {
        if (!count) return;
        if (threads_.empty() || count == 1) {
            for (std::size_t index = 0; index < count; ++index) job(index);
            return;
        }
        // Each batch is its own: a thread still leaving the last one finds nothing in it to take.
        const auto batch = std::make_shared<Batch>(&job, count);
        {
            std::lock_guard lock(mutex_);
            batch_ = batch;
            ++number_;
        }
        wake_.notify_all();
        take(*batch);
        std::unique_lock lock(mutex_);
        done_.wait(lock, [&] { return !batch->left; });
        batch_.reset();
    }

  private:
    struct Batch {
        Batch(const std::function<void(std::size_t)> *work, std::size_t jobs) : job(work), count(jobs), left(jobs) {}
        const std::function<void(std::size_t)> *job;
        const std::size_t count;
        std::atomic<std::size_t> next{0};
        std::size_t left; // under the pool's mutex
    };
    void take(Batch &batch) {
        for (;;) {
            const auto index = batch.next.fetch_add(1, std::memory_order_relaxed);
            if (index >= batch.count) return;
            (*batch.job)(index);
            std::lock_guard lock(mutex_);
            if (!--batch.left) done_.notify_all();
        }
    }
    void work() {
        std::uint64_t seen{};
        for (;;) {
            std::shared_ptr<Batch> batch;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || number_ != seen; });
                if (stopping_) return;
                seen = number_;
                batch = batch_;
            }
            if (batch) take(*batch);
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::shared_ptr<Batch> batch_;
    std::uint64_t number_{};
    bool stopping_{};
    std::vector<std::jthread> threads_; // last: joined before the rest goes
};
} // namespace dingosdk::server
