#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace noisefactor::sync::companion {

// Callbacks bound for the thread that owns the companion's UI, queued here
// rather than carried inside the platform's wake-up message. A callback
// queued just before the window is destroyed is then still run by the final
// drain instead of dying with its message, which would leave the helper
// supervisor's destructor waiting on an operation that never completes.
//
// Thread model: dispatch() is called from any thread; drain() and
// mark_owner_gone() are called on the owner thread. Callbacks never run
// under the lock, so a callback may dispatch again.
class OwnerDispatchQueue {
 public:
  using Callback = std::function<void()>;

  // Queues the callback and asks `wake` to nudge the owner thread. If the
  // wake cannot be delivered, or the owner is already gone, the callback runs
  // inline on the calling thread instead so that it is never dropped.
  template <class Wake>
  void dispatch(Callback callback, Wake&& wake) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!owner_gone_) {
        queue_.push_back(std::move(callback));
        if (wake()) return;
        callback = std::move(queue_.back());
        queue_.pop_back();
      }
    }
    callback();
  }

  // Runs every queued callback on the calling thread, including any queued
  // while draining.
  void drain() {
    for (;;) {
      Callback callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return;
        callback = std::move(queue_.front());
        queue_.pop_front();
      }
      callback();
    }
  }

  // After this, dispatch() runs callbacks inline. Call on the owner thread
  // once its message loop has ended, then drain() what was queued before.
  void mark_owner_gone() {
    std::lock_guard<std::mutex> lock(mutex_);
    owner_gone_ = true;
  }

  [[nodiscard]] bool owner_gone() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owner_gone_;
  }

  [[nodiscard]] std::size_t pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<Callback> queue_;
  bool owner_gone_ = false;
};

}  // namespace noisefactor::sync::companion
