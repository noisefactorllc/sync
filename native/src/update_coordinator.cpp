#include <sync/update_coordinator.hpp>
#include <limits>

namespace noisefactor::sync::update {
bool valid_digest(std::string_view value) noexcept {
  if (value.size()!=64) return false;
  for (const char c:value) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
  return true;
}
bool Coordinator::observe_clock(std::uint64_t now) {
  if (now < latest_ms_) return false;
  latest_ms_=now;
  return true;
}
void Coordinator::expire(std::uint64_t now) {
  if (token_ && !committed_ && now>=prepared_ms_ && now-prepared_ms_>=kPreparationMs) {
    token_=0;digest_.clear();
    idle_since_=external_idle_&&activities_==0 ? std::optional(now) : std::nullopt;
  }
}
void Coordinator::set_external_state(bool idle,std::uint64_t now) {
  std::lock_guard lock(mutex_);
  const bool valid_clock = observe_clock(now);
  if ((!idle || !valid_clock) && !committed_) {
    token_ = 0;
    digest_.clear();
  }
  if(!valid_clock) { external_idle_=false;idle_since_.reset();return; }
  expire(now);
  if(!idle) idle_since_.reset();
  else if(!external_idle_&&activities_==0) idle_since_=now;
  external_idle_=idle;
}
bool Coordinator::ready(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if (!observe_clock(now)) return false;
  expire(now);
  return !token_ && activities_ == 0 && external_idle_ && idle_since_ &&
      now >= *idle_since_ && now - *idle_since_ >= kIdleMs;
}
bool Coordinator::begin_activity(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if(!observe_clock(now)) return false;
  expire(now);
  if(token_||activities_==std::numeric_limits<std::size_t>::max()) return false;
  ++activities_;idle_since_.reset();return true;
}
void Coordinator::end_activity(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  const bool valid=observe_clock(now);
  // Even an invalid clock must release activity, but it cannot establish idle.
  if(activities_==0) {external_idle_=false;idle_since_.reset();return;}
  --activities_;
  if(!valid) {external_idle_=false;idle_since_.reset();return;}
  if(activities_==0&&external_idle_) idle_since_=now;
}
std::optional<std::uint64_t> Coordinator::reserve(std::string_view digest,std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if(!observe_clock(now)||!valid_digest(digest)) return std::nullopt;
  expire(now);
  if(token_||activities_||!external_idle_||!idle_since_||now<*idle_since_||now-*idle_since_<kIdleMs||sequence_==std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  digest_=digest;token_=++sequence_;prepared_ms_=now;committed_=false;return token_;
}
bool Coordinator::commit(std::uint64_t token,std::string_view digest,std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if(!observe_clock(now)) return false;
  expire(now);
  if(!token||token!=token_||digest!=digest_||!external_idle_||activities_||
     !idle_since_||now<*idle_since_||now-*idle_since_<kIdleMs) return false;
  committed_=true;return true;
}
bool Coordinator::cancel(std::uint64_t token,std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if(!token||token!=token_) return false;
  const bool valid=observe_clock(now);
  token_=0;committed_=false;digest_.clear();
  idle_since_=valid&&external_idle_&&activities_==0 ? std::optional(now) : std::nullopt;
  return true;
}
bool Coordinator::reserved() const {std::lock_guard lock(mutex_);return token_!=0;}
}
