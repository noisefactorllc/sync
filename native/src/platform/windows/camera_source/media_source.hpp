#pragma once

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

#include <sync/platform/camera_relay_policy.hpp>

#include "section_owner.hpp"

namespace noisefactor::sync::camera {

class SyncCameraSource;

// The one stream the camera exposes. Pulls the newest frame out of the shared
// ring on each RequestSample, converts it to whatever the consumer negotiated,
// and falls back to the waiting card when no sender is feeding it.
class SyncCameraStream final : public IMFMediaStream {
 public:
  SyncCameraStream(SyncCameraSource* source, IMFStreamDescriptor* descriptor) noexcept;

  // IUnknown
  auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) -> HRESULT override;
  auto STDMETHODCALLTYPE AddRef() -> ULONG override;
  auto STDMETHODCALLTYPE Release() -> ULONG override;

  // IMFMediaEventGenerator
  auto STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** event) -> HRESULT override;
  auto STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
      -> HRESULT override;
  auto STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
      -> HRESULT override;
  auto STDMETHODCALLTYPE QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                                    const PROPVARIANT* value) -> HRESULT override;

  // IMFMediaStream
  auto STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** source) -> HRESULT override;
  auto STDMETHODCALLTYPE GetStreamDescriptor(IMFStreamDescriptor** descriptor)
      -> HRESULT override;
  auto STDMETHODCALLTYPE RequestSample(IUnknown* token) -> HRESULT override;

  [[nodiscard]] auto Start() -> HRESULT;
  [[nodiscard]] auto Stop() -> HRESULT;
  void Shutdown();

 private:
  ~SyncCameraStream();

  [[nodiscard]] auto CurrentSubtype(GUID& subtype) -> HRESULT;
  // Fills bgra_ with the frame to send: the newest real frame when one is
  // available, the last one again while inside the relay policy's grace
  // period, and the waiting card once that expires.
  void ComposeFrame();
  [[nodiscard]] auto WrapAsSample(std::uint64_t presentation_time, IMFSample** sample)
      -> HRESULT;

  std::atomic<ULONG> references_{1};
  // Weak on purpose: the source owns the stream, so a strong reference here
  // would be a cycle neither ever escapes.
  SyncCameraSource* source_ = nullptr;
  Microsoft::WRL::ComPtr<IMFStreamDescriptor> descriptor_;
  Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;

  std::mutex mutex_;
  bool shutdown_ = false;
  bool started_ = false;
  SectionOwner section_;
  CameraRelayPolicy policy_;
  std::vector<std::byte> bgra_;
  std::vector<std::byte> idle_card_;
  std::vector<std::byte> converted_;
  std::uint64_t last_ring_sequence_ = 0;
};

// The media source the frame server activates. One device, one stream, live.
class SyncCameraSource final : public IMFMediaSource {
 public:
  SyncCameraSource() noexcept;

  [[nodiscard]] auto Initialize() -> HRESULT;

  // IUnknown
  auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) -> HRESULT override;
  auto STDMETHODCALLTYPE AddRef() -> ULONG override;
  auto STDMETHODCALLTYPE Release() -> ULONG override;

  // IMFMediaEventGenerator
  auto STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** event) -> HRESULT override;
  auto STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
      -> HRESULT override;
  auto STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
      -> HRESULT override;
  auto STDMETHODCALLTYPE QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                                    const PROPVARIANT* value) -> HRESULT override;

  // IMFMediaSource
  auto STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) -> HRESULT override;
  auto STDMETHODCALLTYPE CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor)
      -> HRESULT override;
  auto STDMETHODCALLTYPE Start(IMFPresentationDescriptor* descriptor, const GUID* time_format,
                               const PROPVARIANT* start_position) -> HRESULT override;
  auto STDMETHODCALLTYPE Stop() -> HRESULT override;
  auto STDMETHODCALLTYPE Pause() -> HRESULT override;
  auto STDMETHODCALLTYPE Shutdown() -> HRESULT override;

 private:
  ~SyncCameraSource();

  std::atomic<ULONG> references_{1};
  std::mutex mutex_;
  bool shutdown_ = false;
  bool started_ = false;
  Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;
  Microsoft::WRL::ComPtr<IMFPresentationDescriptor> presentation_;
  Microsoft::WRL::ComPtr<SyncCameraStream> stream_;
};

}  // namespace noisefactor::sync::camera
