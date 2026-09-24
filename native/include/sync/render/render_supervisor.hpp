#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <uv.h>

#include <sync/frame_receiver.hpp>
#include <sync/render/render_source.hpp>

namespace noisefactor::sync::render {

struct RenderSupervisorOptions {
  // The sync-render executable and the arguments that choose what it renders
  // (--join, --seance-url, --width, ...). The supervisor adds --ring and
  // --exit-on-stdin-eof itself; they are its half of the contract.
  std::string helper_path;
  std::vector<std::string> helper_arguments;
  std::string ring_name;
  std::string sender_id = "render";
  std::string sender_name = "Sync Render";
  // How often the ring is checked for a new frame. Well under a 60 fps frame
  // period, so a frame waits at most this long between the helper finishing
  // it and the providers receiving it.
  std::uint64_t poll_interval_ms = 2;
  std::uint64_t restart_base_ms = 500;
  std::uint64_t restart_max_ms = 30'000;
};

// Runs sync-render as a child of syncd on syncd's own libuv loop: spawns it,
// reads its event lines, attaches a RenderSource to its ring when it reports
// ready, polls that ring on the loop thread, and restarts the helper with
// backoff when it exits for a reason a restart can fix.
//
// The helper's stdin is a pipe syncd holds open. Closing it is the stop
// request, and if syncd dies the OS closes it for us, so the helper never
// outlives the daemon on any platform.
class RenderSupervisor {
 public:
  RenderSupervisor(uv_loop_t* loop, FramePublisher& publisher, RenderSupervisorOptions options);
  ~RenderSupervisor();
  RenderSupervisor(const RenderSupervisor&) = delete;
  auto operator=(const RenderSupervisor&) -> RenderSupervisor& = delete;

  // Starts the first helper and the poll timer. False when the loop handles
  // could not be created; a helper that fails to launch is retried instead.
  [[nodiscard]] auto start() -> bool;
  // Detaches, asks the helper to exit, and closes every handle. The loop
  // must keep running until the handles have closed, as it does during
  // syncd's shutdown.
  void stop();

  struct Status {
    std::uint64_t launches = 0;
    std::uint64_t exits = 0;
    std::int64_t last_exit_status = 0;
    bool running = false;
    bool attached = false;
    bool given_up = false;  // the helper exited for a reason a restart cannot fix
  };
  [[nodiscard]] auto status() const noexcept -> const Status& { return status_; }
  [[nodiscard]] auto source() const noexcept -> const RenderSource& { return source_; }

 private:
  struct Child;

  void launch();
  void schedule_restart();
  void on_line(std::string_view line);
  void on_child_exit(std::int64_t exit_status);
  void poll();

  uv_loop_t* loop_;
  RenderSupervisorOptions options_;
  RenderSource source_;
  std::unique_ptr<Child> child_;
  uv_timer_t* poll_timer_ = nullptr;
  uv_timer_t* restart_timer_ = nullptr;
  std::uint64_t restart_attempt_ = 0;
  bool stopping_ = false;
  Status status_{};
};

// Exit codes sync-render uses for outcomes a restart cannot change: a bad
// command line and a session that ended for good (kicked, banned, locked,
// unknown). Any other exit is retried.
inline constexpr std::int64_t kRenderHelperUsageExit = 2;
inline constexpr std::int64_t kRenderHelperSessionEndedExit = 4;

}  // namespace noisefactor::sync::render
