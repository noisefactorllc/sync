#include <sync/render/render_supervisor.hpp>

#include <algorithm>
#include <csignal>
#include <iostream>
#include <utility>

namespace noisefactor::sync::render {

// One helper process and its pipes. Owned by libuv from spawn until the last
// of its three handles has closed, because a close callback can run after the
// supervisor has already moved on to a replacement (or been destroyed).
struct RenderSupervisor::Child {
  RenderSupervisor* owner = nullptr;
  uv_process_t process{};
  uv_pipe_t in{};
  uv_pipe_t out{};
  int open_handles = 0;
  bool exited = false;
  std::string pending;  // a partial event line waiting for its newline

  void close_all() {
    for (uv_handle_t* handle : {reinterpret_cast<uv_handle_t*>(&in),
                                reinterpret_cast<uv_handle_t*>(&out),
                                reinterpret_cast<uv_handle_t*>(&process)}) {
      if (handle->data != nullptr && !uv_is_closing(handle)) {
        uv_close(handle, [](uv_handle_t* closed) {
          auto* child = static_cast<Child*>(closed->data);
          if (--child->open_handles == 0) delete child;
        });
      }
    }
  }
};

namespace {

constexpr std::size_t kMaximumEventLine = 64 * 1024;

void delete_timer(uv_timer_t* timer) {
  if (timer == nullptr) return;
  uv_timer_stop(timer);
  uv_close(reinterpret_cast<uv_handle_t*>(timer),
           [](uv_handle_t* handle) { delete reinterpret_cast<uv_timer_t*>(handle); });
}

}  // namespace

RenderSupervisor::RenderSupervisor(uv_loop_t* loop, FramePublisher& publisher,
                                   RenderSupervisorOptions options)
    : loop_(loop),
      options_(std::move(options)),
      source_(publisher, options_.sender_id, options_.sender_name) {}

RenderSupervisor::~RenderSupervisor() {
  stop();
}

auto RenderSupervisor::start() -> bool {
  poll_timer_ = new uv_timer_t{};
  restart_timer_ = new uv_timer_t{};
  if (uv_timer_init(loop_, poll_timer_) != 0) {
    delete poll_timer_;
    poll_timer_ = nullptr;
    delete restart_timer_;
    restart_timer_ = nullptr;
    return false;
  }
  if (uv_timer_init(loop_, restart_timer_) != 0) {
    delete restart_timer_;
    restart_timer_ = nullptr;
    delete_timer(poll_timer_);
    poll_timer_ = nullptr;
    return false;
  }
  poll_timer_->data = this;
  restart_timer_->data = this;
  if (uv_timer_start(
          poll_timer_,
          [](uv_timer_t* timer) { static_cast<RenderSupervisor*>(timer->data)->poll(); },
          options_.poll_interval_ms, options_.poll_interval_ms) != 0) {
    return false;
  }
  launch();
  return true;
}

void RenderSupervisor::stop() {
  if (stopping_) return;
  stopping_ = true;
  source_.detach();
  status_.attached = false;
  delete_timer(poll_timer_);
  poll_timer_ = nullptr;
  delete_timer(restart_timer_);
  restart_timer_ = nullptr;
  if (child_ != nullptr) {
    Child* child = child_.release();
    child->owner = nullptr;
#if !defined(_WIN32)
    // Closing stdin (below) is the portable stop request. On POSIX a SIGTERM
    // as well makes it prompt even if the helper is busy compiling; on
    // Windows uv_process_kill would be TerminateProcess, which skips the
    // helper's orderly ring close, so the pipe alone has to do it there.
    if (!child->exited) uv_process_kill(&child->process, SIGTERM);
#endif
    child->close_all();
  }
  status_.running = false;
}

void RenderSupervisor::launch() {
  if (stopping_) return;
  auto child = std::make_unique<Child>();
  child->owner = this;
  if (uv_pipe_init(loop_, &child->in, 0) != 0) return schedule_restart();
  child->in.data = child.get();
  ++child->open_handles;
  if (uv_pipe_init(loop_, &child->out, 0) != 0) {
    child.release()->close_all();
    return schedule_restart();
  }
  child->out.data = child.get();
  ++child->open_handles;

  std::vector<std::string> arguments;
  arguments.reserve(options_.helper_arguments.size() + 5);
  arguments.push_back(options_.helper_path);
  for (const std::string& argument : options_.helper_arguments) arguments.push_back(argument);
  arguments.emplace_back("--ring");
  arguments.push_back(options_.ring_name);
  arguments.emplace_back("--exit-on-stdin-eof");
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (std::string& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);

  uv_stdio_container_t stdio[3];
  stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
  stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(&child->in);
  stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&child->out);
  // The helper's diagnostics for people go straight to syncd's own stderr.
  stdio[2].flags = UV_INHERIT_FD;
  stdio[2].data.fd = 2;

  uv_process_options_t spawn{};
  spawn.file = options_.helper_path.c_str();
  spawn.args = argv.data();
  spawn.stdio_count = 3;
  spawn.stdio = stdio;
  spawn.flags = UV_PROCESS_WINDOWS_HIDE;
  spawn.exit_cb = [](uv_process_t* process, int64_t exit_status, int term_signal) {
    auto* child = static_cast<Child*>(process->data);
    child->exited = true;
    if (child->owner != nullptr) {
      child->owner->on_child_exit(term_signal != 0 ? -term_signal : exit_status);
    }
  };
  child->process.data = child.get();
  ++child->open_handles;
  const int spawned = uv_spawn(loop_, &child->process, &spawn);
  if (spawned != 0) {
    std::cerr << "syncd: render helper failed to start: " << uv_strerror(spawned) << '\n';
    // A failed spawn still leaves an initialized handle that must be closed.
    child->exited = true;
    child.release()->close_all();
    return schedule_restart();
  }
  ++status_.launches;
  status_.running = true;

  const int reading = uv_read_start(
      reinterpret_cast<uv_stream_t*>(&child->out),
      [](uv_handle_t*, size_t suggested, uv_buf_t* buffer) {
        buffer->base = new char[suggested];
        buffer->len = static_cast<decltype(buffer->len)>(suggested);
      },
      [](uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) {
        auto* child = static_cast<Child*>(stream->data);
        if (count > 0 && child->owner != nullptr) {
          child->pending.append(buffer->base, static_cast<std::size_t>(count));
          std::size_t newline;
          while ((newline = child->pending.find('\n')) != std::string::npos) {
            const std::string line = child->pending.substr(0, newline);
            child->pending.erase(0, newline + 1);
            child->owner->on_line(line);
            if (child->owner == nullptr) break;
          }
          // A helper that never ends a line cannot grow syncd's memory.
          if (child->pending.size() > kMaximumEventLine) child->pending.clear();
        } else if (count < 0) {
          uv_read_stop(stream);
        }
        delete[] buffer->base;
      });
  if (reading != 0) {
    std::cerr << "syncd: render helper output unreadable: " << uv_strerror(reading) << '\n';
  }
  child_ = std::move(child);
}

void RenderSupervisor::on_line(std::string_view line) {
  // The helper writes compact JSON with sorted keys (QJsonDocument), so the
  // event name appears verbatim. syncd needs only "ready"; everything but
  // the once-a-second stats is worth keeping in syncd's log.
  if (line.find("\"event\":\"stats\"") == std::string_view::npos) {
    std::cerr << "syncd: render " << line << '\n';
  }
  if (line.find("\"event\":\"ready\"") == std::string_view::npos) return;
  std::string error;
  if (source_.attach(options_.ring_name, error)) {
    status_.attached = true;
    restart_attempt_ = 0;
  } else {
    std::cerr << "syncd: render ring attach failed: " << error << '\n';
  }
}

void RenderSupervisor::on_child_exit(std::int64_t exit_status) {
  ++status_.exits;
  status_.last_exit_status = exit_status;
  status_.running = false;
  source_.detach();
  status_.attached = false;
  if (child_ != nullptr) {
    Child* child = child_.release();
    child->owner = nullptr;
    child->close_all();
  }
  if (stopping_) return;
  if (exit_status == kRenderHelperUsageExit || exit_status == kRenderHelperSessionEndedExit) {
    status_.given_up = true;
    std::cerr << "syncd: render helper exited " << exit_status << "; not restarting\n";
    return;
  }
  schedule_restart();
}

void RenderSupervisor::schedule_restart() {
  if (stopping_ || restart_timer_ == nullptr) return;
  const std::uint64_t shift = std::min<std::uint64_t>(restart_attempt_, 16);
  const std::uint64_t delay =
      std::min<std::uint64_t>(options_.restart_base_ms << shift, options_.restart_max_ms);
  ++restart_attempt_;
  uv_timer_start(
      restart_timer_,
      [](uv_timer_t* timer) { static_cast<RenderSupervisor*>(timer->data)->launch(); }, delay, 0);
}

void RenderSupervisor::poll() {
  const RenderSource::Poll result = source_.poll(render_clock_us());
  if (result == RenderSource::Poll::WriterGone) {
    status_.attached = false;
    std::cerr << "syncd: render ring " << render_poll_name(result) << '\n';
    // A helper that is still running but has stopped stamping its heartbeat
    // is hung, not idle (it heartbeats every tick even with no program).
    // Ending it routes recovery through the ordinary exit-and-restart path.
    if (child_ != nullptr && !child_->exited) uv_process_kill(&child_->process, SIGTERM);
  }
}

}  // namespace noisefactor::sync::render
