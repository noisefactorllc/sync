#include <sync/update_coordinator.hpp>
#include <limits>

namespace noisefactor::sync::update {
bool valid_digest(std::string_view value) noexcept {
  if (value.size()!=64) return false;
  for (const char c:value) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
  return true;
}
std::uint64_t Coordinator::observe_clock(std::uint64_t now) {
  // The daemon loop and the control service each read the monotonic clock
  // before taking the mutex, so a caller can arrive holding a reading older
  // than one already observed. Order it after that observation instead of
  // refusing it: the latest reading was taken before this call, so idle time
  // measured from it is never overstated.
  if (now > latest_ms_) latest_ms_=now;
  return latest_ms_;
}
void Coordinator::expire(std::uint64_t now) {
  if (token_ && !committed_ && now>=prepared_ms_ && now-prepared_ms_>=kPreparationMs) {
    token_=0;digest_.clear();
    idle_since_=external_idle_&&activities_==0 ? std::optional(now) : std::nullopt;
  }
}
void Coordinator::set_external_state(bool idle,std::uint64_t now) {
  std::lock_guard lock(mutex_);
  now = observe_clock(now);
  if (!idle && !committed_) {
    token_ = 0;
    digest_.clear();
  }
  expire(now);
  if(!idle) idle_since_.reset();
  else if(!external_idle_&&activities_==0) idle_since_=now;
  external_idle_=idle;
}
bool Coordinator::ready(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  now = observe_clock(now);
  expire(now);
  return !token_ && activities_ == 0 && external_idle_ && idle_since_ &&
      now >= *idle_since_ && now - *idle_since_ >= kIdleMs;
}
bool Coordinator::begin_activity(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  now = observe_clock(now);
  expire(now);
  if(token_||activities_==std::numeric_limits<std::size_t>::max()) return false;
  ++activities_;idle_since_.reset();return true;
}
void Coordinator::end_activity(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  now = observe_clock(now);
  // An unbalanced release means the activity count cannot be trusted.
  if(activities_==0) {external_idle_=false;idle_since_.reset();return;}
  --activities_;
  if(activities_==0&&external_idle_) idle_since_=now;
}
std::optional<std::uint64_t> Coordinator::reserve(std::string_view digest,std::uint64_t now) {
  std::lock_guard lock(mutex_);
  now = observe_clock(now);
  if(!valid_digest(digest)) return std::nullopt;
  expire(now);
  if(token_||activities_||!external_idle_||!idle_since_||now<*idle_since_||now-*idle_since_<kIdleMs||sequence_==std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  digest_=digest;token_=++sequence_;prepared_ms_=now;committed_=false;return token_;
}
bool Coordinator::commit(std::uint64_t token,std::string_view digest,std::uint64_t now) {
  std::lock_guard lock(mutex_);
  now = observe_clock(now);
  expire(now);
  if(!token||token!=token_||digest!=digest_||!external_idle_||activities_||
     !idle_since_||now<*idle_since_||now-*idle_since_<kIdleMs) return false;
  committed_=true;return true;
}
bool Coordinator::cancel(std::uint64_t token,std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if(!token||token!=token_) return false;
  now = observe_clock(now);
  token_=0;committed_=false;digest_.clear();
  idle_since_=external_idle_&&activities_==0 ? std::optional(now) : std::nullopt;
  return true;
}
bool Coordinator::reserved() const {std::lock_guard lock(mutex_);return token_!=0;}
}
