#include <sync/platform/camera_publisher.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include <sync/platform/camera_frame_fitter.hpp>
#include <sync/platform/camera_identity.hpp>

namespace noisefactor::sync::camera {

struct CameraFramePublisher::Impl {
  struct SenderEntry {
    bool occupied = false;
    std::uint64_t opened_at = 0;
    std::size_t sender_id_length = 0;
    std::array<char, kMaximumSenderIdBytes> sender_id{};

    [[nodiscard]] auto id_view() const noexcept -> std::string_view {
      return {sender_id.data(), sender_id_length};
    }
  };

  explicit Impl(CameraSink& sink_ref)
      : sink(sink_ref),
        canvas_stride(static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel),
        canvas(canvas_stride * kCanvas.height) {}

  CameraSink& sink;
  std::uint64_t open_counter = 0;
  std::size_t canvas_stride;
  std::vector<std::byte> canvas;
  std::array<SenderEntry, kMaximumSenderEntries> senders{};

  [[nodiscard]] auto find(std::string_view id) noexcept -> SenderEntry* {
    for (SenderEntry& entry : senders) {
      if (entry.occupied && entry.id_view() == id) return &entry;
    }
    return nullptr;
  }

  [[nodiscard]] auto driving() const noexcept -> const SenderEntry* {
    const SenderEntry* oldest = nullptr;
    for (const SenderEntry& entry : senders) {
      if (!entry.occupied) continue;
      if (oldest == nullptr || entry.opened_at < oldest->opened_at) oldest = &entry;
    }
    return oldest;
  }
};

CameraFramePublisher::CameraFramePublisher(CameraSink& sink)
    : impl_(std::make_unique<Impl>(sink)) {}

CameraFramePublisher::~CameraFramePublisher() = default;

auto CameraFramePublisher::available() const noexcept -> bool {
  return impl_->sink.available();
}

auto CameraFramePublisher::unavailable_reason() const noexcept -> CameraSinkUnavailableReason {
  return impl_->sink.unavailable_reason();
}

auto CameraFramePublisher::unavailable_status() const noexcept -> std::int32_t {
  return impl_->sink.unavailable_status();
}

auto CameraFramePublisher::driving_sender() const noexcept -> std::string_view {
  const Impl::SenderEntry* entry = impl_->driving();
  return entry == nullptr ? std::string_view{} : entry->id_view();
}

auto CameraFramePublisher::open_sender(std::string_view sender_id,
                                       std::string_view name) noexcept -> bool {
  if (sender_id.empty() || sender_id.size() > kMaximumSenderIdBytes || name.empty()) {
    return false;
  }
  if (impl_->find(sender_id) != nullptr) return false;
  for (Impl::SenderEntry& entry : impl_->senders) {
    if (entry.occupied) continue;
    std::copy(sender_id.begin(), sender_id.end(), entry.sender_id.begin());
    entry.sender_id_length = sender_id.size();
    entry.opened_at = ++impl_->open_counter;
    entry.occupied = true;
    return true;
  }
  return false;
}

void CameraFramePublisher::close_sender(std::string_view sender_id) noexcept {
  Impl::SenderEntry* entry = impl_->find(sender_id);
  if (entry == nullptr) return;
  *entry = Impl::SenderEntry{};
}

auto CameraFramePublisher::publish(std::string_view sender_id,
                                   const protocol::FrameView& frame) noexcept -> PublishResult {
  const Impl::SenderEntry* entry = impl_->find(sender_id);
  if (entry == nullptr) return PublishResult::Failed;
  if (!available()) return PublishResult::Failed;
  // Only the oldest sender drives the camera; the rest are accepted and
  // dropped so their Syphon and NDI publication is unaffected.
  if (entry != impl_->driving()) return PublishResult::Accepted;
  if (!fit_camera_frame(frame, impl_->canvas, impl_->canvas_stride, kCanvas)) {
    return PublishResult::Failed;
  }
  const CameraSinkFrame fitted{
      .width = kCanvas.width,
      .height = kCanvas.height,
      .row_stride = impl_->canvas_stride,
      .bgra = impl_->canvas,
      .presentation_time_us = frame.presentation_time_us,
  };
  switch (impl_->sink.submit(fitted)) {
    case CameraSinkSubmit::Accepted:
      return PublishResult::Accepted;
    case CameraSinkSubmit::Backpressured:
      return PublishResult::Backpressured;
    case CameraSinkSubmit::Failed:
      return PublishResult::Failed;
  }
  return PublishResult::Failed;
}

auto CameraFramePublisher::poll_failure(std::uint64_t now_ms) noexcept
    -> std::optional<ProviderFailure> {
  (void)now_ms;
  return std::nullopt;
}

}  // namespace noisefactor::sync::camera
