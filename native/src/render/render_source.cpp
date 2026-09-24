#include <sync/render/render_source.hpp>

#include <utility>

namespace noisefactor::sync::render {

RenderSource::RenderSource(FramePublisher& publisher, std::string sender_id, std::string name,
                           std::uint64_t writer_timeout_us)
    : publisher_(publisher),
      sender_id_(std::move(sender_id)),
      name_(std::move(name)),
      writer_timeout_us_(writer_timeout_us) {}

RenderSource::~RenderSource() { detach(); }

auto RenderSource::attach(const std::string& section_name, std::string& error) -> bool {
  detach();
  auto section = RenderRingSection::open(section_name, error);
  if (!section.has_value()) return false;
  RenderRingReader reader(section->bytes());
  if (!reader.valid()) {
    error = "section is not a render ring this syncd understands";
    return false;
  }
  if (reader.writer_closed()) {
    error = "render ring writer has already closed";
    return false;
  }
  if (!publisher_.open_sender(sender_id_, name_)) {
    error = "publisher refused the render sender";
    return false;
  }
  sender_open_ = true;
  section_ = std::move(section);
  reader_.emplace(reader);
  // Frames rendered before this attach are history: publish from the next
  // newest onward, and count nothing as skipped until then.
  last_published_frame_ = 0;
  return true;
}

void RenderSource::detach() noexcept {
  if (sender_open_) {
    publisher_.close_sender(sender_id_);
    sender_open_ = false;
  }
  reader_.reset();
  section_.reset();
  last_published_frame_ = 0;
}

auto RenderSource::poll(std::uint64_t now_us) noexcept -> Poll {
  if (!reader_.has_value()) return Poll::Detached;
  reader_->heartbeat(now_us);
  if (!reader_->writer_alive(now_us, writer_timeout_us_)) {
    detach();
    return Poll::WriterGone;
  }
  const std::uint64_t newest = reader_->newest_frame();
  if (newest == 0) return Poll::NoFrame;
  if (newest == last_published_frame_) {
    ++stats_.repeats;
    return Poll::Repeat;
  }

  RenderFrameLease lease = reader_->acquire();
  if (!lease.held()) {
    ++stats_.lease_misses;
    return Poll::LeaseUnavailable;
  }
  const RenderFrameInfo& info = lease.info();
  const auto payload = lease.payload();
  const protocol::FrameView frame{
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = kRenderPixelFormatRgba8,
      .color_space = info.color_space,
      .alpha_mode = info.alpha_mode,
      .width = info.width,
      .height = info.height,
      .row_stride = info.row_stride,
      .payload_bytes = static_cast<std::uint32_t>(payload.size()),
      .sequence = info.frame,
      .presentation_time_us = info.presentation_time_us,
      .top_down = true,
      .payload = payload,
  };
  // Publishers copy synchronously before returning (FramePublisher contract),
  // so the lease only has to outlive this call.
  const PublishResult result = publisher_.publish(sender_id_, frame);
  lease.release();

  switch (result) {
    case PublishResult::Accepted:
      if (last_published_frame_ != 0 && info.frame > last_published_frame_ + 1) {
        stats_.skipped_frames += info.frame - last_published_frame_ - 1;
      }
      last_published_frame_ = info.frame;
      ++stats_.published;
      return Poll::Published;
    case PublishResult::Backpressured:
      ++stats_.backpressured;
      return Poll::Backpressured;
    case PublishResult::Failed:
      ++stats_.failed;
      return Poll::PublishFailed;
  }
  return Poll::PublishFailed;
}

}  // namespace noisefactor::sync::render
