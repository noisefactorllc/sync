#include "test_harness.hpp"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <sync/camera/frame_ring.hpp>
#include <sync/camera/nv12.hpp>
#include <sync/platform/camera_identity.hpp>

#include "../../src/platform/windows/camera_source/source_guids.hpp"

namespace {

using Microsoft::WRL::ComPtr;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;

using DllGetClassObjectFn = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);

// Loads the DLL and starts Media Foundation once for the whole binary. A
// function-local static rather than a namespace-scope object so this cannot
// race the test registry's own static initialization.
struct Environment {
  HMODULE module = nullptr;
  Environment() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    module = ::LoadLibraryW(L"SyncCamera.dll");
  }
};

[[nodiscard]] auto environment() -> Environment& {
  static Environment instance;
  return instance;
}

// Instantiates the media source straight out of the DLL. This is the whole
// point of the in-process test: no frame server, no registration, no
// elevation, so it runs on any Windows runner.
[[nodiscard]] auto CreateSource() -> ComPtr<IMFMediaSource> {
  Environment& env = environment();
  SYNC_REQUIRE(env.module != nullptr);
  auto entry =
      reinterpret_cast<DllGetClassObjectFn>(::GetProcAddress(env.module, "DllGetClassObject"));
  SYNC_REQUIRE(entry != nullptr);
  ComPtr<IClassFactory> factory;
  SYNC_REQUIRE(SUCCEEDED(entry(kSyncCameraSourceClsid, IID_PPV_ARGS(&factory))));
  ComPtr<IMFMediaSource> source;
  SYNC_REQUIRE(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&source))));
  return source;
}

[[nodiscard]] auto StreamDescriptorOf(const ComPtr<IMFMediaSource>& source)
    -> ComPtr<IMFStreamDescriptor> {
  ComPtr<IMFPresentationDescriptor> presentation;
  SYNC_REQUIRE(SUCCEEDED(source->CreatePresentationDescriptor(&presentation)));
  BOOL selected = FALSE;
  ComPtr<IMFStreamDescriptor> stream;
  SYNC_REQUIRE(SUCCEEDED(presentation->GetStreamDescriptorByIndex(0, &selected, &stream)));
  return stream;
}

SYNC_TEST(the_source_advertises_nv12_first_then_rgb32) {
  auto source = CreateSource();
  ComPtr<IMFPresentationDescriptor> presentation;
  SYNC_REQUIRE(SUCCEEDED(source->CreatePresentationDescriptor(&presentation)));
  DWORD streams = 0;
  SYNC_REQUIRE(SUCCEEDED(presentation->GetStreamDescriptorCount(&streams)));
  SYNC_REQUIRE(streams == 1);

  ComPtr<IMFMediaTypeHandler> handler;
  SYNC_REQUIRE(SUCCEEDED(StreamDescriptorOf(source)->GetMediaTypeHandler(&handler)));
  DWORD types = 0;
  SYNC_REQUIRE(SUCCEEDED(handler->GetMediaTypeCount(&types)));
  SYNC_REQUIRE(types == 2);

  const GUID expected[2] = {MFVideoFormat_NV12, MFVideoFormat_RGB32};
  for (DWORD index = 0; index < types; ++index) {
    ComPtr<IMFMediaType> type;
    SYNC_REQUIRE(SUCCEEDED(handler->GetMediaTypeByIndex(index, &type)));
    GUID subtype{};
    SYNC_REQUIRE(SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)));
    SYNC_REQUIRE(::IsEqualGUID(subtype, expected[index]) != 0);
    UINT32 width = 0;
    UINT32 height = 0;
    SYNC_REQUIRE(SUCCEEDED(::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)));
    SYNC_REQUIRE(width == kCanvas.width && height == kCanvas.height);
  }
}

SYNC_TEST(the_source_reports_a_sixty_fps_frame_rate) {
  auto source = CreateSource();
  ComPtr<IMFMediaTypeHandler> handler;
  SYNC_REQUIRE(SUCCEEDED(StreamDescriptorOf(source)->GetMediaTypeHandler(&handler)));
  ComPtr<IMFMediaType> type;
  SYNC_REQUIRE(SUCCEEDED(handler->GetMediaTypeByIndex(0, &type)));
  UINT32 numerator = 0;
  UINT32 denominator = 0;
  SYNC_REQUIRE(
      SUCCEEDED(::MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)));
  SYNC_REQUIRE(denominator != 0 && numerator / denominator == 60);
}

SYNC_TEST(the_source_is_live) {
  auto source = CreateSource();
  DWORD characteristics = 0;
  SYNC_REQUIRE(SUCCEEDED(source->GetCharacteristics(&characteristics)));
  SYNC_REQUIRE((characteristics & MFMEDIASOURCE_IS_LIVE) != 0);
}

SYNC_TEST(the_source_starts_and_stops_cleanly) {
  auto source = CreateSource();
  ComPtr<IMFPresentationDescriptor> presentation;
  SYNC_REQUIRE(SUCCEEDED(source->CreatePresentationDescriptor(&presentation)));
  PROPVARIANT start;
  ::PropVariantInit(&start);
  start.vt = VT_EMPTY;
  SYNC_REQUIRE(SUCCEEDED(source->Start(presentation.Get(), nullptr, &start)));
  SYNC_REQUIRE(SUCCEEDED(source->Stop()));
  SYNC_REQUIRE(SUCCEEDED(source->Shutdown()));
  ::PropVariantClear(&start);
}

SYNC_TEST(a_shut_down_source_refuses_further_calls) {
  auto source = CreateSource();
  SYNC_REQUIRE(SUCCEEDED(source->Shutdown()));
  ComPtr<IMFPresentationDescriptor> presentation;
  SYNC_REQUIRE(source->CreatePresentationDescriptor(&presentation) == MF_E_SHUTDOWN);
  DWORD characteristics = 0;
  SYNC_REQUIRE(source->GetCharacteristics(&characteristics) == MF_E_SHUTDOWN);
}

SYNC_TEST(a_started_stream_delivers_the_waiting_card_when_no_sender_is_live) {
  auto source = CreateSource();
  ComPtr<IMFPresentationDescriptor> presentation;
  SYNC_REQUIRE(SUCCEEDED(source->CreatePresentationDescriptor(&presentation)));
  PROPVARIANT start;
  ::PropVariantInit(&start);
  start.vt = VT_EMPTY;
  SYNC_REQUIRE(SUCCEEDED(source->Start(presentation.Get(), nullptr, &start)));
  ::PropVariantClear(&start);

  // MENewStream carries the stream itself as its value.
  ComPtr<IMFMediaEvent> event;
  SYNC_REQUIRE(SUCCEEDED(source->GetEvent(0, &event)));
  MediaEventType type = MEUnknown;
  SYNC_REQUIRE(SUCCEEDED(event->GetType(&type)));
  SYNC_REQUIRE(type == MENewStream);
  PROPVARIANT value;
  ::PropVariantInit(&value);
  SYNC_REQUIRE(SUCCEEDED(event->GetValue(&value)));
  SYNC_REQUIRE(value.vt == VT_UNKNOWN && value.punkVal != nullptr);
  ComPtr<IMFMediaStream> stream;
  SYNC_REQUIRE(SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&stream))));
  ::PropVariantClear(&value);

  SYNC_REQUIRE(SUCCEEDED(stream->RequestSample(nullptr)));

  // Drain the stream's queue until the sample arrives; MEStreamStarted is
  // queued ahead of it.
  ComPtr<IMFSample> sample;
  for (int attempt = 0; attempt < 8 && !sample; ++attempt) {
    ComPtr<IMFMediaEvent> stream_event;
    SYNC_REQUIRE(SUCCEEDED(stream->GetEvent(0, &stream_event)));
    MediaEventType stream_type = MEUnknown;
    SYNC_REQUIRE(SUCCEEDED(stream_event->GetType(&stream_type)));
    if (stream_type != MEMediaSample) continue;
    PROPVARIANT sample_value;
    ::PropVariantInit(&sample_value);
    SYNC_REQUIRE(SUCCEEDED(stream_event->GetValue(&sample_value)));
    SYNC_REQUIRE(sample_value.vt == VT_UNKNOWN && sample_value.punkVal != nullptr);
    SYNC_REQUIRE(SUCCEEDED(sample_value.punkVal->QueryInterface(IID_PPV_ARGS(&sample))));
    ::PropVariantClear(&sample_value);
  }
  SYNC_REQUIRE(sample);

  // NV12 is the negotiated default, so the sample is one NV12 frame.
  DWORD length = 0;
  SYNC_REQUIRE(SUCCEEDED(sample->GetTotalLength(&length)));
  SYNC_REQUIRE(length == noisefactor::sync::camera::nv12_size_bytes(
                             kCanvas.width, kCanvas.height, kCanvas.width));

  ComPtr<IMFMediaBuffer> buffer;
  SYNC_REQUIRE(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)));
  BYTE* data = nullptr;
  DWORD current = 0;
  SYNC_REQUIRE(SUCCEEDED(buffer->Lock(&data, nullptr, &current)));
  // The waiting card is dark but not black, and it has lighter text on it, so
  // the luma plane is neither uniform nor at the studio-range floor.
  std::uint8_t brightest = 0;
  std::uint8_t darkest = 255;
  for (DWORD i = 0; i < kCanvas.width * kCanvas.height; ++i) {
    brightest = static_cast<std::uint8_t>(brightest > data[i] ? brightest : data[i]);
    darkest = static_cast<std::uint8_t>(darkest < data[i] ? darkest : data[i]);
  }
  buffer->Unlock();
  SYNC_REQUIRE(darkest > 16);
  SYNC_REQUIRE(brightest > darkest + 40);

  SYNC_REQUIRE(SUCCEEDED(source->Shutdown()));
}

}  // namespace
