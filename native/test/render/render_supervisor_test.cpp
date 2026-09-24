#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
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

}  // namespace
