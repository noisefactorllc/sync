// A stand-in NDI runtime for the publisher tests. It exports the vendor's
// dynamic-load entry point, NDIlib_v5_load, with the table layout of
// Processing.NDI.DynamicLoad.h, and writes one line per video frame to the
// file named by SYNC_NDI_STUB_LOG, so a test can read what a receiver would
// be told about each frame. On POSIX it also writes "loaded" when the loader
// maps it, so a test can tell whether a refused copy was ever loaded. It
// sends nothing anywhere.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define SYNC_NDI_STUB_EXPORT extern "C" __declspec(dllexport)
#else
#define SYNC_NDI_STUB_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

struct SendCreate {
  const char* p_ndi_name;
  const char* p_groups;
  bool clock_video;
  bool clock_audio;
};

// NDIlib_video_frame_v2_t, uncompressed meaning of the stride union.
struct VideoFrame {
  int xres;
  int yres;
  std::int32_t four_cc;
  int frame_rate_n;
  int frame_rate_d;
  float picture_aspect_ratio;
  std::int32_t frame_format_type;
  std::int64_t timecode;
  std::uint8_t* p_data;
  int line_stride_in_bytes;
  const char* p_metadata;
  std::int64_t timestamp;
};

// NDIlib_v5, from initialize through send_send_video_async_v2. The entries
// the publisher never calls stay null.
struct Api {
  bool (*initialize)(void);
  void (*destroy)(void);
  const char* (*version)(void);
  bool (*is_supported_CPU)(void);
  void* reserved_find[4];
  void* (*send_create)(const SendCreate*);
  void (*send_destroy)(void*);
  void* reserved_between[40];
  void (*send_send_video_v2)(void*, const VideoFrame*);
  void (*send_send_video_async_v2)(void*, const VideoFrame*);
};
static_assert(sizeof(Api) == 52 * sizeof(void*));

int sender_token = 0;

void log_line(const char* line) {
#if defined(_MSC_VER)
// MSVC deprecates getenv and fopen in favour of _dupenv_s and fopen_s, which
// MinGW lacks. Neither is unsafe here: the test sets the variable before it
// loads this runtime, and each line is written and closed at once.
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* path = std::getenv("SYNC_NDI_STUB_LOG");
  if (path == nullptr) return;
  if (std::FILE* file = std::fopen(path, "a")) {
    std::fputs(line, file);
    std::fclose(file);
  }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

void log_frame(const char* call, const VideoFrame* frame) {
  char line[256];
  if (frame == nullptr) {
    std::snprintf(line, sizeof line, "%s null\n", call);
  } else {
    const std::uint8_t* first = frame->p_data;
    const std::uint8_t* last_row =
        frame->p_data + static_cast<std::size_t>(frame->yres - 1) *
                            static_cast<std::size_t>(frame->line_stride_in_bytes);
    std::snprintf(line, sizeof line,
                  "%s %d %d %ld %d %d %d %lld %u,%u,%u,%u %u,%u,%u,%u\n", call, frame->xres,
                  frame->yres, static_cast<long>(frame->four_cc), frame->frame_rate_n,
                  frame->frame_rate_d, frame->line_stride_in_bytes,
                  static_cast<long long>(frame->timecode), unsigned{first[0]}, unsigned{first[1]},
                  unsigned{first[2]}, unsigned{first[3]}, unsigned{last_row[0]},
                  unsigned{last_row[1]}, unsigned{last_row[2]}, unsigned{last_row[3]});
  }
  log_line(line);
}

bool initialize() { return true; }
void destroy() {}
const char* version() { return "sync-ndi-stub"; }
bool is_supported_cpu() { return true; }
void* send_create(const SendCreate*) { return &sender_token; }
void send_destroy(void*) {}
void send_video(void*, const VideoFrame* frame) { log_frame("sync", frame); }
void send_video_async(void*, const VideoFrame* frame) { log_frame("async", frame); }

const Api api = [] {
  Api table{};
  table.initialize = initialize;
  table.destroy = destroy;
  table.version = version;
  table.is_supported_CPU = is_supported_cpu;
  table.send_create = send_create;
  table.send_destroy = send_destroy;
  table.send_send_video_v2 = send_video;
  table.send_send_video_async_v2 = send_video_async;
  return table;
}();

#if !defined(_WIN32)
__attribute__((constructor)) void log_load() { log_line("loaded\n"); }
#endif

}  // namespace

SYNC_NDI_STUB_EXPORT const void* NDIlib_v5_load(void) { return &api; }
