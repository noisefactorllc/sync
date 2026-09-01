#pragma once

#include <cstdint>

namespace noisefactor::sync::camera {

// Decides when the extension should synthesize a black frame so the camera
// stays live while no sender is feeding it. A pure state machine with no
// clock of its own: the caller passes host time in nanoseconds.
class CameraRelayPolicy {
 public:
  enum class Action : std::uint8_t { None, EmitBlack };

  static constexpr std::uint64_t kDefaultIdleIntervalNs = 33'333'333;  // 30 fps

  explicit CameraRelayPolicy(std::uint64_t idle_interval_ns = kDefaultIdleIntervalNs) noexcept;

  // A consumer started or stopped the source stream. Counted, so one
  // consumer stopping does not silence a second one still listening.
  void source_started() noexcept;
  void source_stopped() noexcept;

  // A real frame arrived from the daemon and was relayed.
  void client_frame_arrived(std::uint64_t now_ns) noexcept;

  // Called on every timer tick. EmitBlack when a consumer is listening and
  // nothing has been sent for at least the idle interval.
  [[nodiscard]] auto tick(std::uint64_t now_ns) noexcept -> Action;

  [[nodiscard]] auto source_active() const noexcept -> bool;
  [[nodiscard]] auto idle_interval_ns() const noexcept -> std::uint64_t;

 private:
  std::uint64_t idle_interval_ns_;
  std::uint32_t listeners_ = 0;
  std::uint64_t last_frame_ns_ = 0;
  bool has_frame_ = false;
};

}  // namespace noisefactor::sync::camera
