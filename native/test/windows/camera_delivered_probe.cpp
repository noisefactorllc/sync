// Sustained camera-delivery probe (Windows / Media Foundation).
//
// Streams marked frames through the real Sync virtual camera and reads them
// back with an ordinary Media Foundation source reader -- the exact path any
// capture application takes -- to measure DISTINCT delivered frames per second
// over a long run. This is the true-north instrument: it can observe whether
// 1080p60 is actually delivered and sustained, and whether the documented
// multi-hour delivery decline occurs, neither of which the protocol plane can
// show.
//
//   sync_camera_delivered_probe <seconds> <out.jsonl> [target_fps=60]
//
// It writes one JSON object per elapsed second to <out.jsonl> and a final
// "summary" object, so a run that is killed still leaves an interpretable
// trace. It refuses to run, with the exact reason, if the Sync camera is not
// registered and available -- register the camera first (see the report note).
//
// NOT a test and NOT wired into ctest: it needs the registered system camera,
// which is a machine change reserved for an explicit decision. The pure marker
// and accounting logic it uses is unit-tested separately in
// native/test/camera/delivered_marker_test.cpp with no camera at all.

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sync/camera/delivered_marker.hpp>
#include <sync/platform/camera_identity.hpp>
#include <sync/platform/mf_camera_sink.hpp>

namespace {

using Microsoft::WRL::ComPtr;
using namespace noisefactor::sync::camera;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
constexpr std::size_t kCanvasBytes = kStride * kCanvas.height;

[[nodiscard]] std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// A base canvas plus the marker for `seq`. The base is a slow colour ramp by
// second so a human eyeballing the camera sees motion; the marker carries the
// exact sequence for the machine.
void fill_marked(std::vector<std::byte>& bgra, std::uint64_t seq) {
  const std::uint8_t base = static_cast<std::uint8_t>(64 + (seq / 60) % 128);
  for (std::size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i + 0] = std::byte{base};
    bgra[i + 1] = std::byte{static_cast<std::uint8_t>(255 - base)};
    bgra[i + 2] = std::byte{128};
    bgra[i + 3] = std::byte{255};
  }
  encode_marker(bgra, kStride, kCanvas.width, kCanvas.height, seq);
}

[[nodiscard]] ComPtr<IMFMediaSource> activate_sync_camera() {
  ComPtr<IMFAttributes> attributes;
  if (FAILED(::MFCreateAttributes(&attributes, 1))) return nullptr;
  if (FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
    return nullptr;
  }
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  if (FAILED(::MFEnumDeviceSources(attributes.Get(), &devices, &count))) return nullptr;
  ComPtr<IMFMediaSource> source;
  for (UINT32 i = 0; i < count; ++i) {
    wchar_t* name = nullptr;
    UINT32 length = 0;
    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length))) {
      const std::wstring friendly(name, length);
      ::CoTaskMemFree(name);
      if (!source && friendly.find(L"Sync") != std::wstring::npos) {
        devices[i]->ActivateObject(IID_PPV_ARGS(&source));
      }
    }
    devices[i]->Release();
  }
  ::CoTaskMemFree(devices);
  return source;
}

// Reads one sample. Returns the decoded sequence, or nullopt when the reader
// returned no sample (an idle read) or the frame carried no marker.
[[nodiscard]] std::optional<std::uint64_t> read_decoded(const ComPtr<IMFSourceReader>& reader,
                                                        const GUID& subtype) {
  DWORD stream_index = 0, flags = 0;
  LONGLONG timestamp = 0;
  ComPtr<IMFSample> sample;
  if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                &stream_index, &flags, &timestamp, &sample))) {
    return std::nullopt;
  }
  if (!sample) return std::nullopt;
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return std::nullopt;
  BYTE* data = nullptr;
  DWORD length = 0;
  if (FAILED(buffer->Lock(&data, nullptr, &length))) return std::nullopt;

  const bool nv12 = ::IsEqualGUID(subtype, MFVideoFormat_NV12) != 0;
  const std::size_t ystride = kCanvas.width;  // negotiated stride equals width for these formats
  std::optional<std::uint64_t> decoded;
  const std::size_t need = nv12 ? (static_cast<std::size_t>(kCanvas.width) * kCanvas.height)
                                : (kStride * kMarker.height_px());
  if (length >= need) {
    decoded = decode_marker([&](std::uint32_t x, std::uint32_t y) -> std::uint8_t {
      return nv12 ? data[static_cast<std::size_t>(y) * ystride + x]
                  : data[static_cast<std::size_t>(y) * kStride + static_cast<std::size_t>(x) * 4];
    });
  }
  buffer->Unlock();
  return decoded;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: sync_camera_delivered_probe <seconds> <out.jsonl> [target_fps=60]\n");
    return 2;
  }
  const long seconds = std::strtol(argv[1], nullptr, 10);
  const char* out_path = argv[2];
  const std::uint32_t target = argc > 3 ? static_cast<std::uint32_t>(std::strtol(argv[3], nullptr, 10)) : 60;
  if (seconds <= 0) { std::fprintf(stderr, "seconds must be positive\n"); return 2; }

  ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ::MFStartup(MF_VERSION, MFSTARTUP_LITE);

  MfCameraSink sink;
  if (!sink.available()) {
    std::fprintf(stderr, "camera unavailable: %s\n",
                 describe_unavailability(sink.unavailable_reason(), sink.unavailable_status()).c_str());
    std::fprintf(stderr, "register the Sync camera against this build first; the probe was not run.\n");
    return 1;
  }

  ComPtr<IMFMediaSource> source = activate_sync_camera();
  if (!source) { std::fprintf(stderr, "could not activate the Sync camera as a system source\n"); return 1; }
  ComPtr<IMFSourceReader> reader;
  if (FAILED(::MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) {
    std::fprintf(stderr, "could not create a source reader\n");
    return 1;
  }
  GUID subtype = GUID_NULL;
  {
    ComPtr<IMFMediaType> negotiated;
    if (SUCCEEDED(reader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &negotiated))) {
      negotiated->GetGUID(MF_MT_SUBTYPE, &subtype);
    }
  }

  // std::ofstream, not fopen: the CI toolchain treats fopen as C4996 under /WX.
  // Binary mode keeps the newline exactly one byte, so the JSONL stays stable.
  std::ofstream out(out_path, std::ios::binary);
  if (!out) { std::fprintf(stderr, "cannot open %s\n", out_path); return 1; }

  // Producer: submit marked frames at the target cadence on its own thread.
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> submitted{0};
  std::thread producer([&]() {
    std::vector<std::byte> canvas(kCanvasBytes);
    std::uint64_t seq = 0;
    const auto interval = std::chrono::nanoseconds(1'000'000'000LL / target);
    auto next = std::chrono::steady_clock::now();
    while (!stop.load(std::memory_order_relaxed)) {
      fill_marked(canvas, seq);
      const CameraSinkFrame frame{
          .width = kCanvas.width,
          .height = kCanvas.height,
          .row_stride = kStride,
          .bgra = canvas,
          .presentation_time_us = seq * 1'000'000ULL / target,
      };
      if (sink.submit(frame) != CameraSinkSubmit::Failed) {
        ++seq;
        submitted.store(seq, std::memory_order_relaxed);
      }
      next += interval;
      std::this_thread::sleep_until(next);
    }
  });

  DeliveryAccounting acc(target, [&](const DeliverySecond& d) {
    std::array<char, 192> line{};
    std::snprintf(line.data(), line.size(),
        "{\"t\":%llu,\"new\":%u,\"repeat\":%u,\"empty\":%u,\"outOfOrder\":%u}\n",
        static_cast<unsigned long long>(d.second), d.newFrames, d.repeats, d.empties, d.outOfOrder);
    out << line.data();
    out.flush();
  });

  const std::uint64_t start = now_ms();
  const std::uint64_t run_ms = static_cast<std::uint64_t>(seconds) * 1000;
  while (now_ms() - start < run_ms) {
    acc.observe(now_ms(), read_decoded(reader, subtype));
  }
  stop.store(true, std::memory_order_relaxed);
  producer.join();

  const auto sum = acc.finish();
  std::array<char, 512> summary{};
  std::snprintf(summary.data(), summary.size(),
      "{\"type\":\"summary\",\"seconds\":%llu,\"submitted\":%llu,\"totalNew\":%llu,"
      "\"totalRepeats\":%llu,\"totalEmpties\":%llu,\"totalOutOfOrder\":%llu,"
      "\"meanDeliveredFps\":%.2f,\"maxSecondFps\":%u,\"minSecondFps\":%u,"
      "\"sustainedTarget\":%s,\"target\":%u,\"firstThirdFps\":%.2f,\"lastThirdFps\":%.2f}\n",
      static_cast<unsigned long long>(sum.seconds),
      static_cast<unsigned long long>(submitted.load()),
      static_cast<unsigned long long>(sum.totalNew),
      static_cast<unsigned long long>(sum.totalRepeats),
      static_cast<unsigned long long>(sum.totalEmpties),
      static_cast<unsigned long long>(sum.totalOutOfOrder),
      sum.meanDeliveredFps, sum.maxSecondFps, sum.minSecondFps,
      sum.sustainedTarget ? "true" : "false", sum.target, sum.firstThirdFps, sum.lastThirdFps);
  out << summary.data();
  out.close();
  source->Shutdown();

  std::printf("delivered probe done: %llu s, mean %.1f fps, sustained %s\n",
              static_cast<unsigned long long>(sum.seconds), sum.meanDeliveredFps,
              sum.sustainedTarget ? "yes" : "no");
  return 0;
}
