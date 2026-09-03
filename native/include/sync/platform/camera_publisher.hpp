#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <sync/frame_receiver.hpp>
#include <sync/platform/camera_sink.hpp>

namespace noisefactor::sync::camera {

// Direct CPU-frame send provider for the Sync Camera extension.
//
// PublisherHub opens a sender across every provider as a unit, so a provider
// that declined would fail the sender for Syphon and NDI too. A camera is one
// device, though, so this provider accepts every sender and drives the camera
// from the oldest one still open. Frames from the other senders are accepted
// here and dropped; they still reach the other providers.
class CameraFramePublisher final : public FramePublisher {
 public:
  static constexpr std::size_t kMaximumSenderEntries = 8;
  static constexpr std::size_t kMaximumSenderIdBytes = 128;

  explicit CameraFramePublisher(CameraSink& sink);
  ~CameraFramePublisher() override;

  CameraFramePublisher(const CameraFramePublisher&) = delete;
  auto operator=(const CameraFramePublisher&) -> CameraFramePublisher& = delete;
  CameraFramePublisher(CameraFramePublisher&&) = delete;
  auto operator=(CameraFramePublisher&&) -> CameraFramePublisher& = delete;

  [[nodiscard]] auto available() const noexcept -> bool;
  [[nodiscard]] auto unavailable_reason() const noexcept -> CameraSinkUnavailableReason;
  [[nodiscard]] auto unavailable_status() const noexcept -> std::int32_t;
  // Empty when no sender is open.
  [[nodiscard]] auto driving_sender() const noexcept -> std::string_view;

  auto open_sender(std::string_view sender_id, std::string_view name) noexcept -> bool override;
  void close_sender(std::string_view sender_id) noexcept override;
  auto publish(std::string_view sender_id, const protocol::FrameView& frame) noexcept
      -> PublishResult override;
  // Camera trouble is never fatal to the daemon: this always returns nullopt.
  auto poll_failure(std::uint64_t now_ms) noexcept
      -> std::optional<ProviderFailure> override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync::camera
