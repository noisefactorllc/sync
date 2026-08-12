#include <sync/pairing.hpp>

#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <sync/secure_memory.hpp>

namespace noisefactor::sync::pairing {
namespace {

struct AuthorityRequest {
  std::uint64_t generation = 0;
  AuthorityOperation operation = AuthorityOperation::Authenticate;
  NormalizedOrigin origin{};
  std::array<char, kMaximumAuthorityTokenBytes> token{};
  std::size_t token_length = 0;
  std::size_t slot = kMaximumAuthorityRequests;

  AuthorityRequest() = default;
  AuthorityRequest(const AuthorityRequest &) = delete;
  AuthorityRequest &operator=(const AuthorityRequest &) = delete;

  ~AuthorityRequest() noexcept { clear(nullptr); }

  void clear(CleanseObserver *observer) noexcept {
    secure_cleanse(std::as_writable_bytes(std::span(token)), observer);
    generation = 0;
    operation = AuthorityOperation::Authenticate;
    origin = {};
    token_length = 0;
    slot = kMaximumAuthorityRequests;
  }

  void assign_authenticate(std::uint64_t request_generation,
                           const NormalizedOrigin &request_origin,
                           std::string_view request_token,
                           std::size_t request_slot,
                           CleanseObserver *observer) noexcept {
    clear(observer);
    generation = request_generation;
    operation = AuthorityOperation::Authenticate;
    origin = request_origin;
    std::memcpy(token.data(), request_token.data(), request_token.size());
    token_length = request_token.size();
    slot = request_slot;
  }

  void assign_issue(std::uint64_t request_generation,
                    const NormalizedOrigin &request_origin,
                    std::size_t request_slot,
                    CleanseObserver *observer) noexcept {
    clear(observer);
    generation = request_generation;
    operation = AuthorityOperation::Issue;
    origin = request_origin;
    slot = request_slot;
  }

  void take_from(AuthorityRequest &other,
                 CleanseObserver *observer) noexcept {
    clear(observer);
    generation = other.generation;
    operation = other.operation;
    origin = other.origin;
    if (other.token_length != 0) {
      std::memcpy(token.data(), other.token.data(), other.token_length);
      token_length = other.token_length;
    }
    slot = other.slot;
    other.clear(observer);
  }

  [[nodiscard]] std::string_view token_view() const noexcept {
    return {token.data(), token_length};
  }
};

enum class SlotLifecycle { Free, Queued, Active, Result };

struct AuthoritySlot {
  std::uint64_t generation = 0;
  AuthorityOperation operation = AuthorityOperation::Authenticate;
  SlotLifecycle lifecycle = SlotLifecycle::Free;
  bool suppress = false;
  std::optional<PairingCommitGate> gate;
};

} // namespace

struct AuthorityWorker::Impl {
  explicit Impl(PairingAuthority &request_authority,
                CleanseObserver *request_observer)
      : authority(request_authority), cleanse_observer(request_observer),
        thread([this] { run(); }) {}

  ~Impl() noexcept { shutdown(); }

  [[nodiscard]] bool submit_authenticate(
      std::uint64_t generation, const NormalizedOrigin &origin,
      std::string_view token) noexcept {
    if (generation == 0 || origin.empty() || token.empty() ||
        token.size() > kMaximumAuthorityTokenBytes)
      return false;
    std::lock_guard lock(mutex);
    const std::size_t slot = allocate_slot(generation,
                                           AuthorityOperation::Authenticate);
    if (stopping || slot == kMaximumAuthorityRequests)
      return false;
    const std::size_t tail =
        (request_head + request_count) % kMaximumAuthorityRequests;
    requests[tail].assign_authenticate(generation, origin, token, slot,
                                       cleanse_observer);
    ++request_count;
    ++outstanding;
    condition.notify_one();
    return true;
  }

  [[nodiscard]] bool submit_issue(std::uint64_t generation,
                                  const NormalizedOrigin &origin) noexcept {
    if (generation == 0 || origin.empty())
      return false;
    std::lock_guard lock(mutex);
    const std::size_t slot = allocate_slot(generation,
                                           AuthorityOperation::Issue);
    if (stopping || slot == kMaximumAuthorityRequests)
      return false;
    const std::size_t tail =
        (request_head + request_count) % kMaximumAuthorityRequests;
    requests[tail].assign_issue(generation, origin, slot, cleanse_observer);
    ++request_count;
    ++outstanding;
    condition.notify_one();
    return true;
  }

  [[nodiscard]] bool poll(AuthorityResult &result) noexcept {
    std::lock_guard lock(mutex);
    if (result_count == 0)
      return false;
    result = std::move(results[result_head]);
    results[result_head] = {};
    const std::size_t slot = result_slots[result_head];
    result_slots[result_head] = kMaximumAuthorityRequests;
    result_head = (result_head + 1) % kMaximumAuthorityRequests;
    --result_count;
    --outstanding;
    release_slot(slot);
    return true;
  }

  [[nodiscard]] bool has_result(std::uint64_t generation) noexcept {
    if (generation == 0)
      return false;
    std::lock_guard lock(mutex);
    for (const AuthoritySlot &slot : slots) {
      if (slot.lifecycle == SlotLifecycle::Result &&
          slot.generation == generation)
        return true;
    }
    return false;
  }

  [[nodiscard]] bool cancel(std::uint64_t generation) noexcept {
    if (generation == 0)
      return false;
    std::lock_guard lock(mutex);
    std::size_t slot_index = kMaximumAuthorityRequests;
    for (std::size_t index = 0; index < slots.size(); ++index) {
      if (slots[index].lifecycle != SlotLifecycle::Free &&
          slots[index].generation == generation) {
        slot_index = index;
        break;
      }
    }
    if (slot_index == kMaximumAuthorityRequests)
      return false;
    AuthoritySlot &slot = slots[slot_index];
    slot.suppress = true;
    if (slot.gate.has_value())
      (void)slot.gate->cancel();
    if (slot.lifecycle == SlotLifecycle::Queued) {
      for (std::size_t offset = 0; offset < request_count; ++offset) {
        if (requests[(request_head + offset) % kMaximumAuthorityRequests].slot ==
            slot_index) {
          remove_request(offset);
          release_slot(slot_index);
          --outstanding;
          break;
        }
      }
    } else if (slot.lifecycle == SlotLifecycle::Result) {
      for (std::size_t offset = 0; offset < result_count; ++offset) {
        if (result_slots[(result_head + offset) % kMaximumAuthorityRequests] ==
            slot_index) {
          remove_result(offset);
          release_slot(slot_index);
          --outstanding;
          break;
        }
      }
    }
    return true;
  }

  void shutdown() noexcept {
    {
      std::lock_guard lock(mutex);
      if (!stopping) {
        stopping = true;
        for (AuthoritySlot &slot : slots) {
          if (slot.gate.has_value())
            (void)slot.gate->cancel();
          slot.suppress = true;
        }
        for (std::size_t index = 0; index < request_count; ++index) {
          requests[(request_head + index) % kMaximumAuthorityRequests].clear(
              cleanse_observer);
        }
        request_head = 0;
        request_count = 0;
        for (std::size_t index = 0; index < result_count; ++index) {
          results[(result_head + index) % kMaximumAuthorityRequests] = {};
          result_slots[(result_head + index) % kMaximumAuthorityRequests] =
              kMaximumAuthorityRequests;
        }
        result_head = 0;
        result_count = 0;
        for (std::size_t index = 0; index < slots.size(); ++index) {
          if (slots[index].lifecycle != SlotLifecycle::Active)
            release_slot(index);
        }
        outstanding = active ? 1 : 0;
      }
      condition.notify_all();
    }
    if (thread.joinable())
      thread.join();
  }

  void run() noexcept {
    AuthorityRequest request;
    for (;;) {
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return stopping || request_count != 0; });
        if (stopping)
          return;
        request.take_from(requests[request_head], nullptr);
        request_head = (request_head + 1) % kMaximumAuthorityRequests;
        --request_count;
        active = true;
        slots[request.slot].lifecycle = SlotLifecycle::Active;
      }

      AuthorityResult result;
      result.generation = request.generation;
      result.operation = request.operation;
      if (request.operation == AuthorityOperation::Authenticate) {
        result.authentication =
            authority.authenticate(request.origin, request.token_view());
      } else {
        result.issuance =
            authority.issue(request.origin, *slots[request.slot].gate);
      }
      const std::size_t completed_slot = request.slot;
      request.clear(nullptr);

      {
        std::lock_guard lock(mutex);
        active = false;
        if (stopping || slots[completed_slot].suppress) {
          release_slot(completed_slot);
          --outstanding;
          if (stopping)
            return;
          continue;
        }
        const std::size_t tail =
            (result_head + result_count) % kMaximumAuthorityRequests;
        results[tail] = std::move(result);
        result_slots[tail] = completed_slot;
        ++result_count;
        slots[completed_slot].lifecycle = SlotLifecycle::Result;
        // Delivered under the lock so that clearing the notifier cannot race a
        // call already in flight against a torn-down target.
        if (result_notifier != nullptr) result_notifier(result_notifier_context);
      }
    }
  }

  void set_result_notifier(AuthorityWorker::ResultNotifier notifier,
                           void *context) noexcept {
    std::lock_guard lock(mutex);
    result_notifier = notifier;
    result_notifier_context = context;
  }

  [[nodiscard]] std::size_t allocate_slot(
      std::uint64_t generation, AuthorityOperation operation) noexcept {
    if (stopping || outstanding == kMaximumAuthorityRequests)
      return kMaximumAuthorityRequests;
    for (const AuthoritySlot &slot : slots) {
      if (slot.lifecycle != SlotLifecycle::Free &&
          slot.generation == generation)
        return kMaximumAuthorityRequests;
    }
    for (std::size_t index = 0; index < slots.size(); ++index) {
      AuthoritySlot &slot = slots[index];
      if (slot.lifecycle == SlotLifecycle::Free) {
        slot.generation = generation;
        slot.operation = operation;
        slot.lifecycle = SlotLifecycle::Queued;
        slot.suppress = false;
        slot.gate.emplace();
        return index;
      }
    }
    return kMaximumAuthorityRequests;
  }

  void release_slot(std::size_t index) noexcept {
    if (index >= slots.size())
      return;
    AuthoritySlot &slot = slots[index];
    slot.gate.reset();
    slot.generation = 0;
    slot.operation = AuthorityOperation::Authenticate;
    slot.lifecycle = SlotLifecycle::Free;
    slot.suppress = false;
  }

  void remove_request(std::size_t offset) noexcept {
    for (std::size_t index = offset; index + 1 < request_count; ++index) {
      const std::size_t destination =
          (request_head + index) % kMaximumAuthorityRequests;
      const std::size_t source =
          (request_head + index + 1) % kMaximumAuthorityRequests;
      requests[destination].take_from(requests[source], cleanse_observer);
    }
    const std::size_t last =
        (request_head + request_count - 1) % kMaximumAuthorityRequests;
    requests[last].clear(cleanse_observer);
    --request_count;
  }

  void remove_result(std::size_t offset) noexcept {
    for (std::size_t index = offset; index + 1 < result_count; ++index) {
      const std::size_t destination =
          (result_head + index) % kMaximumAuthorityRequests;
      const std::size_t source =
          (result_head + index + 1) % kMaximumAuthorityRequests;
      results[destination] = std::move(results[source]);
      result_slots[destination] = result_slots[source];
    }
    const std::size_t last =
        (result_head + result_count - 1) % kMaximumAuthorityRequests;
    results[last] = {};
    result_slots[last] = kMaximumAuthorityRequests;
    --result_count;
  }

  PairingAuthority &authority;
  CleanseObserver *cleanse_observer;
  std::mutex mutex;
  std::condition_variable condition;
  std::array<AuthorityRequest, kMaximumAuthorityRequests> requests{};
  std::array<AuthorityResult, kMaximumAuthorityRequests> results{};
  std::array<std::size_t, kMaximumAuthorityRequests> result_slots = [] {
    std::array<std::size_t, kMaximumAuthorityRequests> value{};
    value.fill(kMaximumAuthorityRequests);
    return value;
  }();
  std::array<AuthoritySlot, kMaximumAuthorityRequests> slots{};
  std::size_t request_head = 0;
  std::size_t request_count = 0;
  std::size_t result_head = 0;
  std::size_t result_count = 0;
  std::size_t outstanding = 0;
  bool active = false;
  bool stopping = false;
  AuthorityWorker::ResultNotifier result_notifier = nullptr;
  void *result_notifier_context = nullptr;
  std::thread thread;
};

AuthorityWorker::AuthorityWorker(PairingAuthority &authority,
                                 CleanseObserver *cleanse_observer)
    : impl_(std::make_unique<Impl>(authority, cleanse_observer)) {}

AuthorityWorker::~AuthorityWorker() noexcept = default;

bool AuthorityWorker::submit_authenticate(
    std::uint64_t generation, const NormalizedOrigin &origin,
    std::string_view token) noexcept {
  return impl_ != nullptr &&
         impl_->submit_authenticate(generation, origin, token);
}

bool AuthorityWorker::submit_issue(std::uint64_t generation,
                                   const NormalizedOrigin &origin) noexcept {
  return impl_ != nullptr && impl_->submit_issue(generation, origin);
}

bool AuthorityWorker::poll(AuthorityResult &result) noexcept {
  return impl_ != nullptr && impl_->poll(result);
}

bool AuthorityWorker::has_result(std::uint64_t generation) noexcept {
  return impl_ != nullptr && impl_->has_result(generation);
}

bool AuthorityWorker::cancel(std::uint64_t generation) noexcept {
  return impl_ != nullptr && impl_->cancel(generation);
}

void AuthorityWorker::set_result_notifier(ResultNotifier notifier,
                                          void *context) noexcept {
  if (impl_ != nullptr) impl_->set_result_notifier(notifier, context);
}

void AuthorityWorker::shutdown() noexcept {
  if (impl_ != nullptr)
    impl_->shutdown();
}

} // namespace noisefactor::sync::pairing
