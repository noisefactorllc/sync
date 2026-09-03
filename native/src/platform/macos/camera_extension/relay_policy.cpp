#include <sync/platform/camera_relay_policy.hpp>

namespace noisefactor::sync::camera {

CameraRelayPolicy::CameraRelayPolicy(std::uint64_t idle_interval_ns,
                                     std::uint64_t idle_grace_ns) noexcept
    : idle_interval_ns_(idle_interval_ns == 0 ? kDefaultIdleIntervalNs : idle_interval_ns),
      idle_grace_ns_(idle_grace_ns == 0 ? kDefaultIdleGraceNs : idle_grace_ns) {
  // Grace shorter than the cadence would bring the race back.
  if (idle_grace_ns_ < idle_interval_ns_) idle_grace_ns_ = idle_interval_ns_;
}

void CameraRelayPolicy::source_started() noexcept {
  ++listeners_;
  // A fresh listener should not inherit a stale "recently sent" mark from a
  // session that ended long ago; the first tick decides on its own clock.
  has_sent_ = false;
  has_client_frame_ = false;
}

void CameraRelayPolicy::source_stopped() noexcept {
  if (listeners_ > 0) --listeners_;
}

void CameraRelayPolicy::client_frame_arrived(std::uint64_t now_ns) noexcept {
  last_client_frame_ns_ = now_ns;
  has_client_frame_ = true;
  last_sent_ns_ = now_ns;
  has_sent_ = true;
}

auto CameraRelayPolicy::tick(std::uint64_t now_ns) noexcept -> Action {
  if (listeners_ == 0) return Action::None;
  // A live sender owns the stream: no black while its frames keep coming,
  // even when one lands a few milliseconds behind the idle cadence.
  if (has_client_frame_ && now_ns - last_client_frame_ns_ < idle_grace_ns_) return Action::None;
  if (has_sent_ && now_ns - last_sent_ns_ < idle_interval_ns_) return Action::None;
  if (!has_sent_ && now_ns < idle_interval_ns_) return Action::None;
  last_sent_ns_ = now_ns;
  has_sent_ = true;
  return Action::EmitBlack;
}

auto CameraRelayPolicy::source_active() const noexcept -> bool { return listeners_ > 0; }

auto CameraRelayPolicy::idle_interval_ns() const noexcept -> std::uint64_t {
  return idle_interval_ns_;
}

auto CameraRelayPolicy::idle_grace_ns() const noexcept -> std::uint64_t { return idle_grace_ns_; }

}  // namespace noisefactor::sync::camera
