#include "test_harness.hpp"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <sync/camera/nv12.hpp>
#include <sync/platform/camera_identity.hpp>
#include <sync/platform/mf_camera_sink.hpp>

namespace {

using Microsoft::WRL::ComPtr;
using noisefactor::sync::camera::CameraSinkFrame;
using noisefactor::sync::camera::CameraSinkSubmit;
using noisefactor::sync::camera::CameraSinkUnavailableReason;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::camera::MfCameraSink;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
constexpr std::size_t kCanvasBytes = kStride * kCanvas.height;

struct Environment {
  Environment() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
  }
};

[[nodiscard]] auto environment() -> Environment& {
  static Environment instance;
  return instance;
}

// Finds the Sync camera the way any ordinary application would: enumerate the
// system's video capture devices and match on friendly name. Nothing here
// knows about the shared ring or the CLSID.
[[nodiscard]] auto ActivateSyncCamera() -> ComPtr<IMFMediaSource> {
  ComPtr<IMFAttributes> attributes;
  SYNC_REQUIRE(SUCCEEDED(::MFCreateAttributes(&attributes, 1)));
  SYNC_REQUIRE(SUCCEEDED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)));
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  SYNC_REQUIRE(SUCCEEDED(::MFEnumDeviceSources(attributes.Get(), &devices, &count)));

  ComPtr<IMFMediaSource> source;
  for (UINT32 index = 0; index < count; ++index) {
    wchar_t* name = nullptr;
    UINT32 length = 0;
    if (SUCCEEDED(devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name,
                                                     &length))) {
      const std::wstring friendly(name, length);
      ::CoTaskMemFree(name);
      if (!source && friendly.find(L"Sync") != std::wstring::npos) {
        devices[index]->ActivateObject(IID_PPV_ARGS(&source));
      }
    }
    devices[index]->Release();
  }
  ::CoTaskMemFree(devices);
  return source;
}

// Reads one frame through an ordinary source reader, the same path a capture
// application takes, and reports which format it negotiated. Both advertised
// formats are legitimate answers, and the pixels mean different things in
// each, so the caller has to know which it got.
[[nodiscard]] auto ReadOneFrame(const ComPtr<IMFSourceReader>& reader, std::vector<BYTE>& out,
                                GUID& subtype) -> bool {
  ComPtr<IMFMediaType> negotiated;
  if (SUCCEEDED(reader->GetCurrentMediaType(
          static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &negotiated))) {
    negotiated->GetGUID(MF_MT_SUBTYPE, &subtype);
  }

  for (int attempt = 0; attempt < 60; ++attempt) {
    DWORD stream_index = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                  &stream_index, &flags, &timestamp, &sample))) {
      return false;
    }
    if (!sample) continue;  // a live source answers some reads with nothing yet

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    out.assign(data, data + length);
    buffer->Unlock();
    return true;
  }
  return false;
}

[[nodiscard]] auto canvas_filled(std::uint8_t b, std::uint8_t g, std::uint8_t r)
    -> std::vector<std::byte> {
  std::vector<std::byte> pixels(kCanvasBytes);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<std::byte>(b);
    pixels[i + 1] = static_cast<std::byte>(g);
    pixels[i + 2] = static_cast<std::byte>(r);
    pixels[i + 3] = static_cast<std::byte>(255);
  }
  return pixels;
}

SYNC_TEST(the_sync_camera_is_created_and_enumerable_as_a_system_camera) {
  environment();
  const MfCameraSink sink;
  SYNC_REQUIRE(sink.unavailable_reason() != CameraSinkUnavailableReason::SourceNotRegistered);
  SYNC_REQUIRE(sink.available());

  const auto source = ActivateSyncCamera();
  // The whole point: an application that never heard of Sync finds it by
  // enumerating cameras.
  SYNC_REQUIRE(source);
  source->Shutdown();
}

SYNC_TEST(the_camera_shows_the_waiting_card_with_no_sender_and_the_frame_with_one) {
  environment();
  MfCameraSink sink;
  SYNC_REQUIRE(sink.available());

  const auto source = ActivateSyncCamera();
  SYNC_REQUIRE(source);

  // Whichever format the reader negotiated, the luma-like samples are the
  // first width*height bytes for NV12 and the blue channel of each pixel for
  // RGB32. This walks the right ones for the format in hand.
  const auto scan = [](const std::vector<BYTE>& frame, const GUID& subtype,
                       std::uint8_t& darkest, std::uint8_t& brightest) -> bool {
    const std::size_t pixels = static_cast<std::size_t>(kCanvas.width) * kCanvas.height;
    const bool nv12 = ::IsEqualGUID(subtype, MFVideoFormat_NV12) != 0;
    const std::size_t stride = nv12 ? 1 : 4;
    if (frame.size() < pixels * stride) return false;
    darkest = 255;
    brightest = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
      const BYTE value = frame[i * stride];
      darkest = value < darkest ? value : darkest;
      brightest = value > brightest ? value : brightest;
    }
    return true;
  };

  // One reader for the whole test, as a real consumer would: creating one per
  // frame starts and stops the camera on every read.
  ComPtr<IMFSourceReader> reader;
  SYNC_REQUIRE(SUCCEEDED(::MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader)));

  std::vector<BYTE> idle;
  GUID idle_subtype = GUID_NULL;
  SYNC_REQUIRE(ReadOneFrame(reader, idle, idle_subtype));
  SYNC_REQUIRE(!idle.empty());
  // Both advertised formats are acceptable; anything else means the pipeline
  // negotiated something this source never offered.
  SYNC_REQUIRE(::IsEqualGUID(idle_subtype, MFVideoFormat_NV12) != 0 ||
               ::IsEqualGUID(idle_subtype, MFVideoFormat_RGB32) != 0);

  // The waiting card: dark, but above black, and carrying lighter text. A
  // black frame or a uniform one would both fail here.
  std::uint8_t idle_darkest = 0;
  std::uint8_t idle_brightest = 0;
  SYNC_REQUIRE(scan(idle, idle_subtype, idle_darkest, idle_brightest));
  SYNC_REQUIRE(idle_darkest > 8);
  SYNC_REQUIRE(idle_brightest > idle_darkest + 40);

  // Now feed a solid mid-grey and read again. Grey rather than a primary so
  // the expected value is close in both formats.
  const auto frame = canvas_filled(128, 128, 128);
  const CameraSinkFrame submission{
      .width = kCanvas.width,
      .height = kCanvas.height,
      .row_stride = kStride,
      .bgra = frame,
      .presentation_time_us = 1,
  };
  SYNC_REQUIRE(sink.has_capacity());

  std::vector<BYTE> live;
  bool saw_live = false;
  for (int attempt = 0; attempt < 40 && !saw_live; ++attempt) {
    // Keep feeding: the relay policy falls back to the waiting card once a
    // sender goes quiet, so a single frame could be replaced before the
    // consumer ever reads it.
    for (int fill = 0; fill < 4; ++fill) {
      SYNC_REQUIRE(sink.submit(submission) == CameraSinkSubmit::Accepted);
    }
    GUID live_subtype = GUID_NULL;
    if (!ReadOneFrame(reader, live, live_subtype)) continue;
    std::uint8_t darkest = 0;
    std::uint8_t brightest = 0;
    if (!scan(live, live_subtype, darkest, brightest)) continue;
    // A solid frame: every sample the same, and mid-range rather than the
    // card's dark background. NV12 studio-range grey lands near 126, RGB32
    // passes 128 through untouched.
    saw_live = (brightest - darkest) <= 2 && darkest > 100 && brightest < 160;
  }
  SYNC_REQUIRE(saw_live);
  source->Shutdown();
}

}  // namespace
