#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <sync/frame_receiver.hpp>
#include <sync/render/render_ring.hpp>

namespace noisefactor::sync::render {

// syncd's end of the render ring: an internal sender whose frames come from
// the render helper's shared memory instead of a WebSocket. It opens one
// sender on the publisher while a live ring is attached and publishes each new
// frame straight out of the leased slot, so a rendered frame reaches the
// providers with no copy on this side.
//
// Driven by poll() from syncd's loop thread, the same thread every other
// publish happens on, because providers such as Spout are bound to it.
class RenderSource {
 public:
  enum class Poll : std::uint8_t {
    Detached,        // no ring attached
    NoFrame,         // attached, nothing published yet
    Repeat,          // the newest frame was already published
    Published,       // a new frame reached the publisher
    Backpressured,   // the publisher declined it; the next frame will retry
    PublishFailed,   // the publisher failed
    LeaseUnavailable,  // the writer kept lapping the reader; try next poll
    WriterGone,      // closed or stale; the sender was closed and the ring detached
  };

  struct Stats {
    std::uint64_t published = 0;
    std::uint64_t repeats = 0;
    std::uint64_t skipped_frames = 0;  // frames the writer produced that were never published
    std::uint64_t backpressured = 0;
    std::uint64_t failed = 0;
    std::uint64_t lease_misses = 0;
  };

  RenderSource(FramePublisher& publisher, std::string sender_id, std::string name,
               std::uint64_t writer_timeout_us = 2'000'000);
  ~RenderSource();
  RenderSource(const RenderSource&) = delete;
  auto operator=(const RenderSource&) -> RenderSource& = delete;

  // Opens the named section and opens the sender. Any previous ring is
  // detached first. error explains a false return.
  [[nodiscard]] auto attach(const std::string& section_name, std::string& error) -> bool;
  void detach() noexcept;
  [[nodiscard]] auto attached() const noexcept -> bool { return reader_.has_value(); }

  [[nodiscard]] auto poll(std::uint64_t now_us) noexcept -> Poll;
  [[nodiscard]] auto stats() const noexcept -> const Stats& { return stats_; }
  [[nodiscard]] auto sender_id() const noexcept -> std::string_view { return sender_id_; }

 private:
  FramePublisher& publisher_;
  std::string sender_id_;
  std::string name_;
  std::uint64_t writer_timeout_us_;
  std::optional<RenderRingSection> section_;
  std::optional<RenderRingReader> reader_;
  bool sender_open_ = false;
  std::uint64_t last_published_frame_ = 0;
  Stats stats_{};
};

[[nodiscard]] constexpr auto render_poll_name(RenderSource::Poll poll) noexcept
    -> std::string_view {
  switch (poll) {
    case RenderSource::Poll::Detached: return "detached";
    case RenderSource::Poll::NoFrame: return "no_frame";
    case RenderSource::Poll::Repeat: return "repeat";
    case RenderSource::Poll::Published: return "published";
    case RenderSource::Poll::Backpressured: return "backpressured";
    case RenderSource::Poll::PublishFailed: return "publish_failed";
    case RenderSource::Poll::LeaseUnavailable: return "lease_unavailable";
    case RenderSource::Poll::WriterGone: return "writer_gone";
  }
  return "unknown";
}

}  // namespace noisefactor::sync::render
