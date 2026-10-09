#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

namespace usa::scanner {

// Multi-producer / multi-consumer work queue for scan jobs.
//
// Contract:
// * Every value obtained via pop()/tryPop() must be paired with exactly one
//   taskDone() call once the worker finishes processing it.
// * pop() blocks while the queue is empty or suspended; it returns false
//   only after requestCancel(), at which point workers should exit.
// * waitUntilIdle() unblocks when no pushed task is outstanding, i.e. the
//   queue is empty and nothing is in flight (safe point to shut workers
//   down via requestCancel() without losing work).
// * The destructor requests cancellation so joined workers cannot hang.
template <typename T>
class BlockingQueue {
public:
    BlockingQueue() = default;
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    ~BlockingQueue() { requestCancel(); }

    void push(T value)
    {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            queue_.push(std::move(value));
            ++pending_;
        }
        pushed_.notify_one();
    }

    bool pop(T& out)
    {
        std::unique_lock<std::mutex> lock{mutex_};
        ++waiting_;
        pushed_.wait(lock, [this] {
            return cancelled_.load(std::memory_order_relaxed)
                   || (!suspended_.load(std::memory_order_relaxed) && !queue_.empty());
        });
        --waiting_;
        if (cancelled_ || queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool tryPop(T& out)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (cancelled_.load(std::memory_order_relaxed)
            || suspended_.load(std::memory_order_relaxed) || queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void taskDone()
    {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (pending_ == 0) {
                return;
            }
            --pending_;
        }
        idle_.notify_all();
    }

    void waitUntilIdle()
    {
        // Cancelled counts as idle: after requestCancel(), outstanding items
        // are abandoned (workers exited) and the orchestrator must proceed
        // to join them instead of waiting for taskDone() calls that will
        // never come.
        std::unique_lock<std::mutex> lock{mutex_};
        idle_.wait(lock, [this] {
            return pending_ == 0 || cancelled_.load(std::memory_order_relaxed);
        });
    }

    // Restores a fresh queue. Call only when no worker is inside pop() —
    // i.e. between engine runs, after all threads joined. Suspension is
    // deliberately preserved: pause is UI mode state, not per-scan state.
    void reset()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        queue_ = std::queue<T>{};
        pending_ = 0;
        cancelled_.store(false, std::memory_order_relaxed);
    }

    void setSuspended(bool suspended)
    {
        suspended_.store(suspended, std::memory_order_relaxed);
        // Notify under no lock: waiters re-check the atomic predicate.
        pushed_.notify_all();
    }

    void requestCancel()
    {
        cancelled_.store(true, std::memory_order_relaxed);
        pushed_.notify_all();
        idle_.notify_all();
    }

    // For long-running per-item work: workers call this in tight loops so
    // pause takes effect even mid-item.
    void waitIfSuspended()
    {
        std::unique_lock<std::mutex> lock{mutex_};
        pushed_.wait(lock,
                     [this] { return cancelled_.load(std::memory_order_relaxed)
                                      || !suspended_.load(std::memory_order_relaxed); });
    }

    // Lock-free: called on every file, so it must not serialize on the
    // queue mutex.
    bool isCancelRequested() const { return cancelled_.load(std::memory_order_relaxed); }

    unsigned waitingWorkers() const { return waiting_.load(std::memory_order_relaxed); }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return queue_.size();
    }

private:
    mutable std::mutex mutex_{};
    std::condition_variable pushed_{};
    std::condition_variable idle_{};
    std::queue<T> queue_{};
    std::size_t pending_{0};
    std::atomic<unsigned> waiting_{0};
    // Atomics, not mutex-guarded: isCancelRequested() runs per file and
    // setSuspended()/requestCancel() must wake workers without locking.
    std::atomic<bool> suspended_{false};
    std::atomic<bool> cancelled_{false};
};

} // namespace usa::scanner
