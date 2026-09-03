#pragma once

#include <windows.h>

// Order matters and is not alphabetical on purpose: ks.h must follow the Media
// Foundation headers. Ahead of them its macros reach cguid.h through
// objbase.h and break __uuidof there, which surfaces as a syntax error inside
// a system header rather than anywhere near here.
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>

#include <ks.h>
#include <ksproxy.h>

#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

#include <sync/platform/camera_relay_policy.hpp>

#include "attribute_store.hpp"
#include "module_lock.hpp"
#include "section_owner.hpp"

namespace noisefactor::sync::camera {

class SyncCameraSource;

// The one stream the camera exposes. Pulls the newest frame out of the shared
// ring on each RequestSample, converts it to whatever the consumer negotiated,
// and falls back to the waiting card when no sender is feeding it.
//
// IMFMediaStream2 rather than IMFMediaStream: the frame server requires it of
// every stream on a custom media source. IKsControl and the attribute store
// are required of a stream for the same reason they are of the source -- the
// pipeline asks, and refuses the camera if the answer is E_NOINTERFACE.
class SyncCameraStream final : public IMFMediaStream2,
                               public IKsControl,
                               public AttributeStore<IMFAttributes> {
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

  // IMFMediaStream2
  auto STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE state) -> HRESULT override;
  auto STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE* state) -> HRESULT override;

  // IKsControl
  auto STDMETHODCALLTYPE KsProperty(PKSPROPERTY property, ULONG property_length,
                                    LPVOID property_data, ULONG data_length,
                                    ULONG* bytes_returned) -> HRESULT override;
  auto STDMETHODCALLTYPE KsMethod(PKSMETHOD method, ULONG method_length, LPVOID method_data,
                                  ULONG data_length, ULONG* bytes_returned) -> HRESULT override;
  auto STDMETHODCALLTYPE KsEvent(PKSEVENT event, ULONG event_length, LPVOID event_data,
                                 ULONG data_length, ULONG* bytes_returned) -> HRESULT override;

  [[nodiscard]] auto Initialize() -> HRESULT;
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
  [[nodiscard]] auto StartLocked() -> HRESULT;
  [[nodiscard]] auto StopLocked() -> HRESULT;

  ModuleReference module_reference_;
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
//
// IMFMediaSourceEx, IMFGetService, IKsControl, IMFSampleAllocatorControl and
// an attribute store are all mandatory for a frame server custom media source:
// the pipeline QueryInterfaces for each and refuses the camera with
// E_NOINTERFACE if any is missing.
class SyncCameraSource final : public IMFMediaSourceEx,
                               public IMFGetService,
                               public IKsControl,
                               public IMFSampleAllocatorControl,
                               public AttributeStore<IMFAttributes> {
 public:
  SyncCameraSource() noexcept;

  // Takes the activator's attributes so the source describes this camera the
  // same way the activator did.
  [[nodiscard]] auto Initialize(IMFAttributes* activator_attributes) -> HRESULT;

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

  // IMFMediaSourceEx
  auto STDMETHODCALLTYPE GetSourceAttributes(IMFAttributes** attributes) -> HRESULT override;
  auto STDMETHODCALLTYPE GetStreamAttributes(DWORD stream_identifier, IMFAttributes** attributes)
      -> HRESULT override;
  auto STDMETHODCALLTYPE SetD3DManager(IUnknown* manager) -> HRESULT override;

  // IMFGetService
  auto STDMETHODCALLTYPE GetService(REFGUID service, REFIID riid, LPVOID* object)
      -> HRESULT override;

  // IKsControl
  auto STDMETHODCALLTYPE KsProperty(PKSPROPERTY property, ULONG property_length,
                                    LPVOID property_data, ULONG data_length,
                                    ULONG* bytes_returned) -> HRESULT override;
  auto STDMETHODCALLTYPE KsMethod(PKSMETHOD method, ULONG method_length, LPVOID method_data,
                                  ULONG data_length, ULONG* bytes_returned) -> HRESULT override;
  auto STDMETHODCALLTYPE KsEvent(PKSEVENT event, ULONG event_length, LPVOID event_data,
                                 ULONG data_length, ULONG* bytes_returned) -> HRESULT override;

  // IMFSampleAllocatorControl
  auto STDMETHODCALLTYPE SetDefaultAllocator(DWORD output_stream_identifier,
                                             IUnknown* allocator) -> HRESULT override;
  auto STDMETHODCALLTYPE GetAllocatorUsage(DWORD output_stream_identifier,
                                           DWORD* input_stream_identifier,
                                           MFSampleAllocatorUsage* usage) -> HRESULT override;

 private:
  ~SyncCameraSource();

  ModuleReference module_reference_;
  std::atomic<ULONG> references_{1};
  std::mutex mutex_;
  bool shutdown_ = false;
  bool started_ = false;
  Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;
  Microsoft::WRL::ComPtr<IMFPresentationDescriptor> presentation_;
  Microsoft::WRL::ComPtr<SyncCameraStream> stream_;
};

// What the registered CLSID actually resolves to.
//
// The frame server does not create the media source directly: it creates this,
// asks it for IMFActivate, and calls ActivateObject to get the source. A DLL
// whose class factory hands back the media source itself fails at
// IMFVirtualCamera::Start with E_NOINTERFACE, having never been asked for
// anything the source implements.
class SyncCameraActivator final : public AttributeStore<IMFActivate> {
 public:
  SyncCameraActivator() noexcept = default;

  [[nodiscard]] auto Initialize() -> HRESULT;

  // IUnknown
  auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) -> HRESULT override;
  auto STDMETHODCALLTYPE AddRef() -> ULONG override;
  auto STDMETHODCALLTYPE Release() -> ULONG override;

  // IMFActivate
  auto STDMETHODCALLTYPE ActivateObject(REFIID riid, void** object) -> HRESULT override;
  auto STDMETHODCALLTYPE ShutdownObject() -> HRESULT override;
  auto STDMETHODCALLTYPE DetachObject() -> HRESULT override;

 private:
  ~SyncCameraActivator();

  ModuleReference module_reference_;
  std::atomic<ULONG> references_{1};
  Microsoft::WRL::ComPtr<SyncCameraSource> source_;
};

}  // namespace noisefactor::sync::camera
