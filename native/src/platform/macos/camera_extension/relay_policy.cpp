#include <sync/platform/camera_relay_policy.hpp>

namespace noisefactor::sync::camera {

CameraRelayPolicy::CameraRelayPolicy(std::uint64_t idle_interval_ns) noexcept
    : idle_interval_ns_(idle_interval_ns == 0 ? kDefaultIdleIntervalNs : idle_interval_ns) {}

void CameraRelayPolicy::source_started() noexcept {
  ++listeners_;
  // A fresh listener should not inherit a stale "recently sent" mark from a
  // session that ended long ago; the first tick decides on its own clock.
  has_frame_ = false;
}

void CameraRelayPolicy::source_stopped() noexcept {
  if (listeners_ > 0) --listeners_;
}

void CameraRelayPolicy::client_frame_arrived(std::uint64_t now_ns) noexcept {
  last_frame_ns_ = now_ns;
  has_frame_ = true;
}

auto CameraRelayPolicy::tick(std::uint64_t now_ns) noexcept -> Action {
  if (listeners_ == 0) return Action::None;
  if (has_frame_ && now_ns - last_frame_ns_ < idle_interval_ns_) return Action::None;
  if (!has_frame_ && now_ns < idle_interval_ns_) return Action::None;
  last_frame_ns_ = now_ns;
  has_frame_ = true;
  return Action::EmitBlack;
}

auto CameraRelayPolicy::source_active() const noexcept -> bool { return listeners_ > 0; }

auto CameraRelayPolicy::idle_interval_ns() const noexcept -> std::uint64_t {
  return idle_interval_ns_;
}

}  // namespace noisefactor::sync::camera
