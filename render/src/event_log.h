#pragma once

#include <QJsonObject>
#include <QString>

#include <cstdio>

namespace noisefactor::sync::render_helper {

// The helper's report channel to whoever launched it: one compact JSON object
// per line on stdout, flushed per line so a supervising syncd sees each event
// as it happens. Diagnostics meant for people go to stderr instead, so the
// two never interleave inside a line.
//
// Events (every one carries "event" and "time_us" on the render clock):
//   ready    ring, width, height, fps, loop_seconds, alpha_mode, effects
//   seance   status, detail
//   program  source ("seance" | "file"), rev, ok, compile_ms, error?, diagnostic?
//   render_error  message
//   stats    reader_attached (syncd read the ring within 1 s) and the
//            engine's counters since start
//   stopped  code, reason       (seance ended the session for good)
//   fatal    message            (startup failed; the process exits)
class EventLog {
 public:
  explicit EventLog(std::FILE* out = stdout) : out_(out) {}
  void write(const QString& event, QJsonObject fields = {});

 private:
  std::FILE* out_;
};

}  // namespace noisefactor::sync::render_helper
