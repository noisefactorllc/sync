#include "test_harness.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <uv.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <filesystem>
#include <unistd.h>
#endif

#include <sync/render/render_ring.hpp>
#include <sync/render/render_supervisor.hpp>

namespace {

using namespace noisefactor::sync;
using namespace noisefactor::sync::render;

class CountingPublisher final : public FramePublisher {
 public:
  auto open_sender(std::string_view, std::string_view) noexcept -> bool override {
    ++opened;
    return true;
  }
  void close_sender(std::string_view) noexcept override { ++closed; }
  auto publish(std::string_view, const protocol::FrameView& frame) noexcept
      -> PublishResult override {
    ++published;
    last_width = frame.width;
    return PublishResult::Accepted;
  }
  int opened = 0;
  int closed = 0;
  int published = 0;
  std::uint32_t last_width = 0;
};

[[nodiscard]] auto ring_name(const char* tag) -> std::string {
#if defined(_WIN32)
  return "SyncRenderSupervisorTest-" + std::to_string(::GetCurrentProcessId()) + "-" + tag;
#else
  return (std::filesystem::temp_directory_path() /
          ("sync-render-supervisor-test-" + std::to_string(::getpid()) + "-" + tag))
      .string();
#endif
}

// Runs the loop until pred holds or the deadline passes. A timer re-checks
// the condition every few milliseconds; nothing here waits a fixed time.
[[nodiscard]] auto run_until(uv_loop_t* loop, const std::function<bool()>& pred,
                             int timeout_ms = 10'000) -> bool {
  struct Watch {
    const std::function<bool()>* pred;
    std::chrono::steady_clock::time_point deadline;
    bool met = false;
  } watch{&pred, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)};
  uv_timer_t timer{};
  uv_timer_init(loop, &timer);
  timer.data = &watch;
  uv_timer_start(
      &timer,
      [](uv_timer_t* t) {
        auto* w = static_cast<Watch*>(t->data);
        if ((*w->pred)()) w->met = true;
        if (w->met || std::chrono::steady_clock::now() > w->deadline) uv_stop(t->loop);
      },
      0, 2);
  uv_run(loop, UV_RUN_DEFAULT);
  uv_timer_stop(&timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
  uv_run(loop, UV_RUN_NOWAIT);
  return watch.met || pred();
}

// Lets every closing handle finish, as syncd's shutdown does.
void drain(uv_loop_t* loop) {
  for (int i = 0; i < 200 && uv_loop_alive(loop) != 0; ++i) uv_run(loop, UV_RUN_ONCE);
}

[[nodiscard]] auto options(const char* tag, std::vector<std::string> extra = {})
    -> RenderSupervisorOptions {
  RenderSupervisorOptions options;
  options.helper_path = SYNC_FAKE_RENDER_HELPER;
  options.helper_arguments = std::move(extra);
  options.ring_name = ring_name(tag);
  options.restart_base_ms = 20;
  options.restart_max_ms = 40;
  return options;
}

SYNC_TEST(a_helper_that_reports_ready_is_attached_and_published) {
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    RenderSupervisor supervisor(&loop, publisher, options("ready"));
    SYNC_REQUIRE(supervisor.start());
    SYNC_REQUIRE(run_until(&loop, [&] { return publisher.published >= 5; }));
    SYNC_REQUIRE(supervisor.status().attached);
    SYNC_REQUIRE(supervisor.status().launches == 1);
    SYNC_REQUIRE(publisher.opened == 1);
    SYNC_REQUIRE(publisher.last_width == 32);
    supervisor.stop();
    SYNC_REQUIRE(publisher.closed == 1);
    drain(&loop);
  }
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

SYNC_TEST(a_crashed_helper_is_restarted_and_reattached) {
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    RenderSupervisor supervisor(
        &loop, publisher, options("restart", {"--fake-exit-after", "3", "--fake-exit-code", "3"}));
    SYNC_REQUIRE(supervisor.start());
    SYNC_REQUIRE(run_until(&loop, [&] { return supervisor.status().launches >= 3; }));
    SYNC_REQUIRE(supervisor.status().exits >= 2);
    SYNC_REQUIRE(supervisor.status().last_exit_status == 3);
    SYNC_REQUIRE(!supervisor.status().given_up);
    // Every launch opened the sender afresh and every exit closed it.
    SYNC_REQUIRE(publisher.opened >= 2);
    SYNC_REQUIRE(publisher.closed >= 2);
    supervisor.stop();
    drain(&loop);
  }
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

SYNC_TEST(a_session_that_ended_for_good_is_not_restarted) {
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    RenderSupervisor supervisor(
        &loop, publisher,
        options("ended", {"--fake-exit-after", "2", "--fake-exit-code",
                          std::to_string(kRenderHelperSessionEndedExit)}));
    SYNC_REQUIRE(supervisor.start());
    SYNC_REQUIRE(run_until(&loop, [&] { return supervisor.status().given_up; }));
    SYNC_REQUIRE(supervisor.status().launches == 1);
    SYNC_REQUIRE(!supervisor.status().running);
    supervisor.stop();
    drain(&loop);
  }
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

SYNC_TEST(a_hung_helper_is_ended_and_replaced) {
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    RenderSupervisorOptions hung = options("hung", {"--fake-stall-after", "2"});
    RenderSupervisor supervisor(&loop, publisher, hung);
    SYNC_REQUIRE(supervisor.start());
    // The source's default staleness window is two seconds.
    SYNC_REQUIRE(run_until(&loop, [&] { return supervisor.status().launches >= 2; }, 15'000));
    SYNC_REQUIRE(supervisor.status().exits >= 1);
    supervisor.stop();
    drain(&loop);
  }
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

SYNC_TEST(a_helper_blocked_past_sigterm_is_killed_and_replaced) {
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    // The fake's main loop blocks for good, so a SIGTERM handler that only
    // sets a flag never gets the helper to exit on its own.
    RenderSupervisor supervisor(&loop, publisher, options("blocked", {"--fake-block-after", "2"}));
    SYNC_REQUIRE(supervisor.start());
    SYNC_REQUIRE(run_until(&loop, [&] { return supervisor.status().launches >= 2; }, 15'000));
    SYNC_REQUIRE(supervisor.status().exits >= 1);
#if !defined(_WIN32)
    SYNC_REQUIRE(supervisor.status().last_exit_status == -SIGKILL);
#endif
    supervisor.stop();
    drain(&loop);
  }
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

SYNC_TEST(stopping_a_responsive_helper_lets_it_close_its_ring) {
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    const RenderSupervisorOptions closes = options("closes");
    RenderSupervisor supervisor(&loop, publisher, closes);
    SYNC_REQUIRE(supervisor.start());
    SYNC_REQUIRE(run_until(&loop, [&] { return publisher.published >= 2; }));
    std::string error;
    auto section = RenderRingSection::open(closes.ring_name, error);
    SYNC_REQUIRE(section.has_value());
    const RenderRingReader reader(section->bytes());
    SYNC_REQUIRE(reader.valid());
    SYNC_REQUIRE(!reader.writer_closed());
    supervisor.stop();
    SYNC_REQUIRE(run_until(&loop, [&] { return reader.writer_closed(); }));
    drain(&loop);
  }
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

SYNC_TEST(a_missing_helper_is_retried_without_crashing_syncd) {
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    RenderSupervisorOptions missing = options("missing");
    missing.helper_path = missing.ring_name + "-no-such-helper";
    RenderSupervisor supervisor(&loop, publisher, missing);
    // The spawn fails synchronously inside start(); a restart is scheduled
    // instead of an error escaping into syncd.
    SYNC_REQUIRE(supervisor.start());
    SYNC_REQUIRE(supervisor.status().launches == 0);
    SYNC_REQUIRE(!supervisor.status().running);
    SYNC_REQUIRE(publisher.opened == 0);
    supervisor.stop();
    drain(&loop);
  }
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

// The 1 ms timer resolution is held exactly while a source is attached: taken
// on attach, given back on every way out of it, so the begin and end calls
// stay balanced through exits, relaunches, a stalled writer and stop.
SYNC_TEST(the_timer_resolution_is_held_exactly_while_a_source_is_attached) {
  SYNC_REQUIRE(timer_resolution_holds() == 0);
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    RenderSupervisor supervisor(
        &loop, publisher, options("resolution", {"--fake-exit-after", "3", "--fake-exit-code", "3"}));
    SYNC_REQUIRE(supervisor.start());
    bool balanced = true;
    SYNC_REQUIRE(run_until(&loop, [&] {
      balanced = balanced && timer_resolution_holds() == (supervisor.status().attached ? 1 : 0);
      return supervisor.status().launches >= 3 && supervisor.status().exits >= 2;
    }));
    SYNC_REQUIRE(balanced);
    supervisor.stop();
    SYNC_REQUIRE(timer_resolution_holds() == 0);
    drain(&loop);
  }
  {
    // A writer that stops heartbeating is detached by the poll, not by an exit.
    RenderSupervisor supervisor(&loop, publisher, options("resolution-stall", {"--fake-stall-after", "2"}));
    SYNC_REQUIRE(supervisor.start());
    SYNC_REQUIRE(run_until(&loop, [&] { return supervisor.status().attached; }));
    SYNC_REQUIRE(timer_resolution_holds() == 1);
    SYNC_REQUIRE(run_until(&loop, [&] { return !supervisor.status().attached; }, 15'000));
    SYNC_REQUIRE(timer_resolution_holds() == 0);
    supervisor.stop();
    drain(&loop);
  }
  SYNC_REQUIRE(timer_resolution_holds() == 0);
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

// A helper pacing 60 fps, polled at the real 2 ms interval, is published
// without a skipped frame. On Windows the poll timer runs that fast only with
// the timer resolution the supervisor holds; at the 15.6 ms default it polls
// at about 64 Hz and skips frames, which is what this guards against.
SYNC_TEST(a_60_fps_ring_is_published_without_skips_at_the_real_poll_interval) {
  uv_loop_t loop{};
  SYNC_REQUIRE(uv_loop_init(&loop) == 0);
  CountingPublisher publisher;
  {
    const RenderSupervisorOptions paced = options("paced", {"--fake-fps", "60"});
    SYNC_REQUIRE(paced.poll_interval_ms == 2);
    RenderSupervisor supervisor(&loop, publisher, paced);
    SYNC_REQUIRE(supervisor.start());
    SYNC_REQUIRE(run_until(&loop, [&] { return supervisor.source().stats().published >= 10; }));
    const RenderSource::Stats before = supervisor.source().stats();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    SYNC_REQUIRE(run_until(&loop, [&] { return std::chrono::steady_clock::now() >= until; }, 6'000));
    const RenderSource::Stats after = supervisor.source().stats();
    const std::uint64_t published = after.published - before.published;
    const std::uint64_t polls = published + (after.repeats - before.repeats);
    std::printf("paced: published %llu, skipped %llu, polls %llu in 3 s\n",
                static_cast<unsigned long long>(published),
                static_cast<unsigned long long>(after.skipped_frames - before.skipped_frames),
                static_cast<unsigned long long>(polls));
    // The skip itself is asserted where the timer defect lives. A hosted
    // macOS runner can stall the fake's own pacing for a frame period or
    // more, which is the fake's scheduling, not the supervisor's poll.
#if defined(_WIN32)
    SYNC_REQUIRE(after.skipped_frames == before.skipped_frames);
#endif
    SYNC_REQUIRE(published >= 100);
    // About 1500 polls at 2 ms; about 190 at the Windows default resolution.
    SYNC_REQUIRE(polls >= 600);
    supervisor.stop();
    drain(&loop);
  }
  SYNC_REQUIRE(uv_loop_close(&loop) == 0);
}

}  // namespace
