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
        pushed_.wait(lock, [this] { return cancelled_ || (!suspended_ && !queue_.empty()); });
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
        if (cancelled_ || suspended_ || queue_.empty()) {
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
        std::unique_lock<std::mutex> lock{mutex_};
        idle_.wait(lock, [this] { return pending_ == 0; });
    }

    void setSuspended(bool suspended)
    {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            suspended_ = suspended;
        }
        pushed_.notify_all();
    }

    void requestCancel()
    {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            cancelled_ = true;
        }
        pushed_.notify_all();
        idle_.notify_all();
    }

    // For long-running per-item work: workers call this in tight loops so
    // pause takes effect even mid-item.
    void waitIfSuspended()
    {
        std::unique_lock<std::mutex> lock{mutex_};
        pushed_.wait(lock, [this] { return cancelled_ || !suspended_; });
    }

    bool isCancelRequested() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return cancelled_;
    }

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
    bool suspended_{false};
    bool cancelled_{false};
};

} // namespace usa::scanner
