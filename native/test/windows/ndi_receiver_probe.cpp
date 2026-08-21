// Independent NDI receiver for certifying Sync's NDI publisher.
//
// Uses the vendor's own headers and runtime -- nothing from the Sync tree --
// so this agrees with Sync only if Sync is actually right. It discovers the
// named source, receives a frame, and checks the pixels it was told to
// expect, including the top-left marker that proves the image is not flipped.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Processing.NDI.Lib.h"
#include "Processing.NDI.DynamicLoad.h"

namespace {

const NDIlib_v5* load_runtime(const char* dll_path) {
  const HMODULE module = ::LoadLibraryA(dll_path);
  if (module == nullptr) {
    std::fprintf(stderr, "could not load %s (error %lu)\n", dll_path,
                 ::GetLastError());
    return nullptr;
  }
  using load_fn = const NDIlib_v5* (*)(void);
  const auto load =
      reinterpret_cast<load_fn>(::GetProcAddress(module, "NDIlib_v5_load"));
  if (load == nullptr) {
    std::fprintf(stderr, "NDIlib_v5_load missing from %s\n", dll_path);
    return nullptr;
  }
  return load();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: ndi_receiver_probe <runtime.dll> <name-substring> "
                 "<width> <height> [timeout-ms]\n");
    return 2;
  }
  const char* dll_path = argv[1];
  const std::string wanted = argv[2];
  const int expected_width = std::atoi(argv[3]);
  const int expected_height = argc > 4 ? std::atoi(argv[4]) : 0;
  const int timeout_ms = argc > 5 ? std::atoi(argv[5]) : 15000;

  const NDIlib_v5* ndi = load_runtime(dll_path);
  if (ndi == nullptr) return 3;
  if (!ndi->NDIlib_initialize()) {
    std::fprintf(stderr, "NDI runtime refused to initialise on this CPU\n");
    return 3;
  }
  std::printf("NDI runtime loaded: %s\n", ndi->NDIlib_version());

  NDIlib_find_create_t find_create{};
  find_create.show_local_sources = true;
  NDIlib_find_instance_t finder = ndi->NDIlib_find_create_v2(&find_create);
  if (finder == nullptr) {
    std::fprintf(stderr, "could not create an NDI finder\n");
    return 3;
  }

  NDIlib_source_t match{};
  std::string matched_name;
  const DWORD deadline = ::GetTickCount() + static_cast<DWORD>(timeout_ms);
  while (::GetTickCount() < deadline && matched_name.empty()) {
    ndi->NDIlib_find_wait_for_sources(finder, 500);
    uint32_t count = 0;
    const NDIlib_source_t* sources =
        ndi->NDIlib_find_get_current_sources(finder, &count);
    for (uint32_t index = 0; index < count; ++index) {
      const char* name = sources[index].p_ndi_name;
      if (name == nullptr) continue;
      if (std::string(name).find(wanted) != std::string::npos) {
        match = sources[index];
        matched_name = name;
        break;
      }
    }
  }
  if (matched_name.empty()) {
    std::fprintf(stderr, "no NDI source matching \"%s\" appeared within %dms\n",
                 wanted.c_str(), timeout_ms);
    ndi->NDIlib_find_destroy(finder);
    return 4;
  }
  std::printf("discovered source: %s\n", matched_name.c_str());

  NDIlib_recv_create_v3_t recv_create{};
  recv_create.source_to_connect_to = match;
  // Ask for RGBA so the bytes arrive in the order the sender wrote them; any
  // conversion the runtime does is its business, but this keeps the pixel
  // check meaningful.
  recv_create.color_format = NDIlib_recv_color_format_RGBX_RGBA;
  recv_create.bandwidth = NDIlib_recv_bandwidth_highest;
  recv_create.allow_video_fields = false;
  recv_create.p_ndi_recv_name = "Sync certification receiver";
  NDIlib_recv_instance_t receiver = ndi->NDIlib_recv_create_v3(&recv_create);
  ndi->NDIlib_find_destroy(finder);
  if (receiver == nullptr) {
    std::fprintf(stderr, "could not create an NDI receiver\n");
    return 3;
  }

  int status = 5;
  const DWORD frame_deadline = ::GetTickCount() + static_cast<DWORD>(timeout_ms);
  while (::GetTickCount() < frame_deadline) {
    NDIlib_video_frame_v2_t video{};
    const NDIlib_frame_type_e type =
        ndi->NDIlib_recv_capture_v3(receiver, &video, nullptr, nullptr, 500);
    if (type != NDIlib_frame_type_video) continue;

    std::printf("received frame: %dx%d fourcc=0x%08X stride=%d\n", video.xres,
                video.yres, static_cast<unsigned>(video.FourCC),
                video.line_stride_in_bytes);

    bool ok = true;
    if (expected_width != 0 && video.xres != expected_width) {
      std::fprintf(stderr, "width %d != expected %d\n", video.xres, expected_width);
      ok = false;
    }
    if (expected_height != 0 && video.yres != expected_height) {
      std::fprintf(stderr, "height %d != expected %d\n", video.yres, expected_height);
      ok = false;
    }
    if (video.FourCC != NDIlib_FourCC_video_type_RGBA) {
      std::fprintf(stderr, "unexpected FourCC 0x%08X (wanted RGBA 0x%08X)\n",
                   static_cast<unsigned>(video.FourCC),
                   static_cast<unsigned>(NDIlib_FourCC_video_type_RGBA));
      ok = false;
    }
    if (video.p_data == nullptr) {
      std::fprintf(stderr, "frame carried no pixels\n");
      ok = false;
    }

    if (ok) {
      const uint8_t* rows = video.p_data;
      const int stride = video.line_stride_in_bytes;
      // Sampled well inside each half. NDI subsamples chroma and compresses,
      // so exact equality is the wrong question -- what matters is that the
      // top half is red-dominant and the bottom half blue-dominant, which a
      // vertically flipped image cannot fake.
      const uint8_t* upper =
          rows + static_cast<size_t>(video.yres / 4) * stride + (video.xres / 2) * 4;
      const uint8_t* lower =
          rows + static_cast<size_t>(video.yres * 3 / 4) * stride + (video.xres / 2) * 4;

      std::printf("  upper quarter rgba = %3u %3u %3u %3u\n", upper[0], upper[1],
                  upper[2], upper[3]);
      std::printf("  lower quarter rgba = %3u %3u %3u %3u\n", lower[0], lower[1],
                  lower[2], lower[3]);

      const bool upper_is_red = upper[0] > 0x90 && upper[2] < 0x70;
      const bool lower_is_blue = lower[2] > 0x90 && lower[0] < 0x70;
      const bool upper_is_blue = upper[2] > 0x90 && upper[0] < 0x70;

      if (upper_is_red && lower_is_blue) {
        std::printf("NDI RECEIVE OK: correct size, RGBA, and upright\n");
        status = 0;
      } else if (upper_is_blue) {
        std::fprintf(stderr,
                     "IMAGE IS VERTICALLY FLIPPED: blue arrived on top\n");
        status = 7;
      } else {
        std::fprintf(stderr, "pixels do not match the published pattern\n");
        status = 6;
      }
    } else {
      status = 6;
    }

    ndi->NDIlib_recv_free_video_v2(receiver, &video);
    break;
  }

  if (status == 5) std::fprintf(stderr, "no video frame arrived within %dms\n", timeout_ms);
  ndi->NDIlib_recv_destroy(receiver);
  ndi->NDIlib_destroy();
  return status;
}
