#include "media_source.hpp"

#include <mferror.h>
#include <mftransform.h>
#include <mfvirtualcamera.h>

#include <ksmedia.h>

#include <cstring>
#include <new>

#include <sync/camera/frame_ring.hpp>
#include <sync/camera/nv12.hpp>
#include <sync/platform/camera_identity.hpp>
#include <sync/platform/camera_idle_card.hpp>

#include "source_guids.hpp"

namespace noisefactor::sync::camera {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kCanvasStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
constexpr std::size_t kCanvasBytes = kCanvasStride * kCanvas.height;
// 100ns units, the unit every Media Foundation timestamp uses.
constexpr std::uint64_t kFrameDuration100ns = 10'000'000ULL / kMaximumFramesPerSecond;

// One media type. NV12 and RGB32 differ only in subtype and default stride.
[[nodiscard]] auto MakeMediaType(const GUID& subtype) -> ComPtr<IMFMediaType> {
  ComPtr<IMFMediaType> type;
  if (FAILED(::MFCreateMediaType(&type))) return nullptr;
  if (FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) return nullptr;
  if (FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype))) return nullptr;
  type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
  ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, kCanvas.width, kCanvas.height);
  ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kMaximumFramesPerSecond, 1);
  ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  // Positive stride means top-down, which is what the canvas already is.
  const UINT32 stride =
      ::IsEqualGUID(subtype, MFVideoFormat_NV12) ? kCanvas.width : kCanvas.width * kBytesPerPixel;
  type->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
  type->SetUINT32(MF_MT_SAMPLE_SIZE,
                  ::IsEqualGUID(subtype, MFVideoFormat_NV12)
                      ? static_cast<UINT32>(nv12_size_bytes(kCanvas.width, kCanvas.height,
                                                            kCanvas.width))
                      : static_cast<UINT32>(kCanvasBytes));
  return type;
}

[[nodiscard]] auto SystemTime100ns() noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>(::MFGetSystemTime());
}

}  // namespace

// ---------------------------------------------------------------- stream ---

SyncCameraStream::SyncCameraStream(SyncCameraSource* source,
                                   IMFStreamDescriptor* descriptor) noexcept
    : source_(source), descriptor_(descriptor) {
  ::MFCreateEventQueue(&events_);
}

SyncCameraStream::~SyncCameraStream() = default;

auto SyncCameraStream::Initialize() -> HRESULT {
  if (!events_) return E_FAIL;
  HRESULT hr = InitializeAttributeStore(4);
  if (FAILED(hr)) return hr;
  // What the frame server reads off a stream to place it in the pipeline: a
  // colour capture pin, shareable between frame server clients. Without these
  // the camera fails to start with MF_E_ATTRIBUTENOTFOUND.
  hr = SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
  if (FAILED(hr)) return hr;
  hr = SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
  if (FAILED(hr)) return hr;
  hr = SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
  if (FAILED(hr)) return hr;
  return SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
}

auto SyncCameraStream::QueryInterface(REFIID riid, void** object) -> HRESULT {
  if (object == nullptr) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaStream ||
      riid == IID_IMFMediaStream2) {
    *object = static_cast<IMFMediaStream2*>(this);
    AddRef();
    return S_OK;
  }
  if (riid == __uuidof(IKsControl)) {
    *object = static_cast<IKsControl*>(this);
    AddRef();
    return S_OK;
  }
  if (riid == IID_IMFAttributes) {
    *object = static_cast<IMFAttributes*>(this);
    AddRef();
    return S_OK;
  }
  *object = nullptr;
  return E_NOINTERFACE;
}

// A stream with no camera controls mimics the AVStream framework's answer for
// an unregistered KS operation, the same as the source does.
auto SyncCameraStream::KsProperty(PKSPROPERTY property, ULONG property_length,
                                  LPVOID property_data, ULONG data_length,
                                  ULONG* bytes_returned) -> HRESULT {
  (void)property;
  (void)property_length;
  (void)property_data;
  (void)data_length;
  if (bytes_returned != nullptr) *bytes_returned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

auto SyncCameraStream::KsMethod(PKSMETHOD method, ULONG method_length, LPVOID method_data,
                                ULONG data_length, ULONG* bytes_returned) -> HRESULT {
  (void)method;
  (void)method_length;
  (void)method_data;
  (void)data_length;
  if (bytes_returned != nullptr) *bytes_returned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

auto SyncCameraStream::KsEvent(PKSEVENT event, ULONG event_length, LPVOID event_data,
                               ULONG data_length, ULONG* bytes_returned) -> HRESULT {
  (void)event;
  (void)event_length;
  (void)event_data;
  (void)data_length;
  if (bytes_returned != nullptr) *bytes_returned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

auto SyncCameraStream::AddRef() -> ULONG { return ++references_; }

auto SyncCameraStream::Release() -> ULONG {
  const ULONG remaining = --references_;
  if (remaining == 0) delete this;
  return remaining;
}

auto SyncCameraStream::GetEvent(DWORD flags, IMFMediaEvent** event) -> HRESULT {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    queue = events_;
  }
  // Outside the lock: GetEvent blocks, and holding the lock across it would
  // deadlock every other caller including Shutdown.
  return queue->GetEvent(flags, event);
}

auto SyncCameraStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->BeginGetEvent(callback, state);
}

auto SyncCameraStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->EndGetEvent(result, event);
}

auto SyncCameraStream::QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                                  const PROPVARIANT* value) -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->QueueEventParamVar(type, extended, status, value);
}

auto SyncCameraStream::GetMediaSource(IMFMediaSource** source) -> HRESULT {
  if (source == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_ || source_ == nullptr) return MF_E_SHUTDOWN;
  *source = source_;
  source_->AddRef();
  return S_OK;
}

auto SyncCameraStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) -> HRESULT {
  if (descriptor == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return descriptor_.CopyTo(descriptor);
}

auto SyncCameraStream::CurrentSubtype(GUID& subtype) -> HRESULT {
  ComPtr<IMFMediaTypeHandler> handler;
  const HRESULT hr = descriptor_->GetMediaTypeHandler(&handler);
  if (FAILED(hr)) return hr;
  ComPtr<IMFMediaType> type;
  if (FAILED(handler->GetCurrentMediaType(&type)) || type == nullptr) {
    // Nothing negotiated yet: the first advertised type is what a consumer
    // would get by default, and it is the one this source prefers.
    if (FAILED(handler->GetMediaTypeByIndex(0, &type))) return MF_E_INVALIDMEDIATYPE;
  }
  return type->GetGUID(MF_MT_SUBTYPE, &subtype);
}

void SyncCameraStream::ComposeFrame() {
  const std::uint64_t now_ns = SystemTime100ns() * 100ULL;

  if (section_.open()) {
    const FrameRingReader reader(section_.mapping());
    if (reader.valid()) {
      const std::uint64_t sequence = reader.newest_sequence();
      if (sequence != 0 && sequence != last_ring_sequence_) {
        std::uint64_t presentation = 0;
        if (reader.read(bgra_, kCanvasStride, presentation)) {
          last_ring_sequence_ = sequence;
          policy_.client_frame_arrived(now_ns);
          return;
        }
      }
    }
  }

  // No new frame. Repeat the last one until the relay policy's grace period
  // expires, which is what stops a jittery 30 fps sender from flickering, then
  // fall back to the waiting card.
  if (policy_.tick(now_ns) == CameraRelayPolicy::Action::EmitBlack) {
    std::memcpy(bgra_.data(), idle_card_.data(), kCanvasBytes);
  }
}

auto SyncCameraStream::WrapAsSample(std::uint64_t presentation_time, IMFSample** sample)
    -> HRESULT {
  GUID subtype{};
  HRESULT hr = CurrentSubtype(subtype);
  if (FAILED(hr)) return hr;

  const bool nv12 = ::IsEqualGUID(subtype, MFVideoFormat_NV12);
  const std::size_t bytes =
      nv12 ? nv12_size_bytes(kCanvas.width, kCanvas.height, kCanvas.width) : kCanvasBytes;
  if (converted_.size() < bytes) converted_.resize(bytes);

  if (nv12) {
    if (!bgra_to_nv12(bgra_, kCanvasStride, kCanvas.width, kCanvas.height, converted_,
                      kCanvas.width)) {
      return E_UNEXPECTED;
    }
  } else {
    std::memcpy(converted_.data(), bgra_.data(), kCanvasBytes);
  }

  ComPtr<IMFMediaBuffer> buffer;
  hr = ::MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer);
  if (FAILED(hr)) return hr;
  BYTE* destination = nullptr;
  hr = buffer->Lock(&destination, nullptr, nullptr);
  if (FAILED(hr)) return hr;
  std::memcpy(destination, converted_.data(), bytes);
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(bytes));

  ComPtr<IMFSample> produced;
  hr = ::MFCreateSample(&produced);
  if (FAILED(hr)) return hr;
  hr = produced->AddBuffer(buffer.Get());
  if (FAILED(hr)) return hr;
  produced->SetSampleTime(static_cast<LONGLONG>(presentation_time));
  produced->SetSampleDuration(static_cast<LONGLONG>(kFrameDuration100ns));
  return produced.CopyTo(sample);
}

auto SyncCameraStream::RequestSample(IUnknown* token) -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  if (!started_) return MF_E_INVALIDREQUEST;

  ComposeFrame();

  ComPtr<IMFSample> sample;
  const HRESULT hr = WrapAsSample(SystemTime100ns(), &sample);
  if (FAILED(hr)) return hr;
  if (token != nullptr) {
    const HRESULT attached = sample->SetUnknown(MFSampleExtension_Token, token);
    if (FAILED(attached)) return attached;
  }
  return events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

auto SyncCameraStream::Start() -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  return StartLocked();
}

auto SyncCameraStream::Stop() -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  return StopLocked();
}

// The frame server drives a custom media source's streams through
// IMFMediaStream2::SetStreamState rather than only through the source's own
// Start and Stop, so both routes have to reach the same place.
auto SyncCameraStream::SetStreamState(MF_STREAM_STATE state) -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  switch (state) {
    case MF_STREAM_STATE_RUNNING:
      return StartLocked();
    case MF_STREAM_STATE_STOPPED:
    case MF_STREAM_STATE_PAUSED:
      return StopLocked();
    default:
      return MF_E_INVALID_STATE_TRANSITION;
  }
}

auto SyncCameraStream::GetStreamState(MF_STREAM_STATE* state) -> HRESULT {
  if (state == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  *state = started_ ? MF_STREAM_STATE_RUNNING : MF_STREAM_STATE_STOPPED;
  return S_OK;
}

auto SyncCameraStream::StartLocked() -> HRESULT {
  if (shutdown_) return MF_E_SHUTDOWN;
  if (started_) return S_OK;
  if (bgra_.size() < kCanvasBytes) bgra_.assign(kCanvasBytes, std::byte{});
  if (idle_card_.size() < kCanvasBytes) idle_card_.assign(kCanvasBytes, std::byte{});
  // Drawn once per start rather than per frame: it never changes, and it is
  // the frame every consumer sees before a sender connects.
  (void)draw_camera_idle_card(idle_card_, kCanvasStride, kCanvas);
  std::memcpy(bgra_.data(), idle_card_.data(), kCanvasBytes);
  started_ = true;
  policy_.source_started();
  return events_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

auto SyncCameraStream::StopLocked() -> HRESULT {
  if (shutdown_) return MF_E_SHUTDOWN;
  if (started_) {
    started_ = false;
    policy_.source_stopped();
  }
  return events_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void SyncCameraStream::Shutdown() {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
    started_ = false;
    source_ = nullptr;
    queue = events_;
  }
  if (queue) queue->Shutdown();
}

// ---------------------------------------------------------------- source ---

SyncCameraSource::SyncCameraSource() noexcept { ::MFCreateEventQueue(&events_); }

SyncCameraSource::~SyncCameraSource() = default;

auto SyncCameraSource::Initialize(IMFAttributes* activator_attributes) -> HRESULT {
  if (!events_) return E_FAIL;
  if (FAILED(InitializeAttributeStore(4))) return E_FAIL;
  if (activator_attributes != nullptr) {
    // The activator's attributes describe this camera to the pipeline, and the
    // source is asked for them again later, so it carries the same set.
    const HRESULT copied = activator_attributes->CopyAllItems(attribute_store());
    if (FAILED(copied)) return copied;
  }

  // A camera has to declare which capture profiles it supports before the
  // frame server will start it; without a collection it fails with
  // MF_E_ATTRIBUTENOTFOUND. One legacy profile and one high-frame-rate
  // profile describe what this source actually offers: a single stream at up
  // to 60 fps.
  ComPtr<IMFSensorProfileCollection> profiles;
  HRESULT profile_hr = ::MFCreateSensorProfileCollection(&profiles);
  if (FAILED(profile_hr)) return profile_hr;
  ComPtr<IMFSensorProfile> legacy;
  profile_hr = ::MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacy);
  if (FAILED(profile_hr)) return profile_hr;
  profile_hr = legacy->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
  if (FAILED(profile_hr)) return profile_hr;
  profile_hr = profiles->AddProfile(legacy.Get());
  if (FAILED(profile_hr)) return profile_hr;
  ComPtr<IMFSensorProfile> high_frame_rate;
  profile_hr = ::MFCreateSensorProfile(KSCAMERAPROFILE_HighFrameRate, 0, nullptr,
                                       &high_frame_rate);
  if (FAILED(profile_hr)) return profile_hr;
  profile_hr = high_frame_rate->AddProfileFilter(0, L"((RES==;FRT>=60,1;SUT==))");
  if (FAILED(profile_hr)) return profile_hr;
  profile_hr = profiles->AddProfile(high_frame_rate.Get());
  if (FAILED(profile_hr)) return profile_hr;
  profile_hr = SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get());
  if (FAILED(profile_hr)) return profile_hr;

  // NV12 first: most Media Foundation consumers want it, and the order here is
  // the preference order a consumer sees.
  ComPtr<IMFMediaType> types[2] = {MakeMediaType(MFVideoFormat_NV12),
                                   MakeMediaType(MFVideoFormat_RGB32)};
  if (!types[0] || !types[1]) return E_FAIL;

  IMFMediaType* raw[2] = {types[0].Get(), types[1].Get()};
  ComPtr<IMFStreamDescriptor> descriptor;
  HRESULT hr = ::MFCreateStreamDescriptor(0, 2, raw, &descriptor);
  if (FAILED(hr)) return hr;

  ComPtr<IMFMediaTypeHandler> handler;
  hr = descriptor->GetMediaTypeHandler(&handler);
  if (FAILED(hr)) return hr;
  hr = handler->SetCurrentMediaType(types[0].Get());
  if (FAILED(hr)) return hr;

  stream_ = new (std::nothrow) SyncCameraStream(this, descriptor.Get());
  if (!stream_) return E_OUTOFMEMORY;
  // The constructor starts the refcount at 1 and ComPtr assignment added
  // another, so drop ours: the ComPtr is the only owner.
  stream_->Release();
  hr = stream_->Initialize();
  if (FAILED(hr)) return hr;

  IMFStreamDescriptor* descriptors[1] = {descriptor.Get()};
  hr = ::MFCreatePresentationDescriptor(1, descriptors, &presentation_);
  if (FAILED(hr)) return hr;
  return presentation_->SelectStream(0);
}

auto SyncCameraSource::QueryInterface(REFIID riid, void** object) -> HRESULT {
  if (object == nullptr) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaSource ||
      riid == IID_IMFMediaSourceEx) {
    *object = static_cast<IMFMediaSourceEx*>(this);
    AddRef();
    return S_OK;
  }
  if (riid == IID_IMFGetService) {
    *object = static_cast<IMFGetService*>(this);
    AddRef();
    return S_OK;
  }
  if (riid == __uuidof(IKsControl)) {
    *object = static_cast<IKsControl*>(this);
    AddRef();
    return S_OK;
  }
  if (riid == IID_IMFSampleAllocatorControl) {
    *object = static_cast<IMFSampleAllocatorControl*>(this);
    AddRef();
    return S_OK;
  }
  if (riid == IID_IMFAttributes) {
    *object = static_cast<IMFAttributes*>(this);
    AddRef();
    return S_OK;
  }
  *object = nullptr;
  return E_NOINTERFACE;
}

auto SyncCameraSource::SetDefaultAllocator(DWORD output_stream_identifier, IUnknown* allocator)
    -> HRESULT {
  (void)output_stream_identifier;
  (void)allocator;
  // This source makes its own buffers with MFCreateMemoryBuffer, so there is
  // no pipeline allocator to adopt.
  return E_NOTIMPL;
}

auto SyncCameraSource::GetAllocatorUsage(DWORD output_stream_identifier,
                                         DWORD* input_stream_identifier,
                                         MFSampleAllocatorUsage* usage) -> HRESULT {
  if (input_stream_identifier == nullptr || usage == nullptr) return E_POINTER;
  if (output_stream_identifier != 0) return MF_E_INVALIDSTREAMNUMBER;
  *input_stream_identifier = 0;
  *usage = MFSampleAllocatorUsage_UsesCustomAllocator;
  return S_OK;
}

auto SyncCameraSource::GetSourceAttributes(IMFAttributes** attributes) -> HRESULT {
  if (attributes == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  *attributes = static_cast<IMFAttributes*>(this);
  AddRef();
  return S_OK;
}

auto SyncCameraSource::GetStreamAttributes(DWORD stream_identifier, IMFAttributes** attributes)
    -> HRESULT {
  if (attributes == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  // One stream, identifier 0, matching the stream descriptor built in
  // Initialize.
  if (stream_identifier != 0 || !stream_) return MF_E_INVALIDSTREAMNUMBER;
  return stream_->QueryInterface(IID_PPV_ARGS(attributes));
}

auto SyncCameraSource::SetD3DManager(IUnknown* manager) -> HRESULT {
  (void)manager;
  // Frames arrive as CPU memory from syncd and are delivered as CPU memory, so
  // there is no D3D device to share. Refusing is the honest answer and the
  // pipeline falls back to system memory, which is what it would have done
  // anyway.
  return E_NOTIMPL;
}

auto SyncCameraSource::GetService(REFGUID service, REFIID riid, LPVOID* object) -> HRESULT {
  (void)service;
  (void)riid;
  if (object != nullptr) *object = nullptr;
  // Mandatory interface, no services to offer.
  return MF_E_UNSUPPORTED_SERVICE;
}

// ERROR_SET_NOT_FOUND is what the AVStream framework returns when a miniport
// driver registers no handler for a KS operation. A source with no camera
// controls mimics that rather than inventing a failure of its own.
auto SyncCameraSource::KsProperty(PKSPROPERTY property, ULONG property_length,
                                  LPVOID property_data, ULONG data_length,
                                  ULONG* bytes_returned) -> HRESULT {
  (void)property;
  (void)property_length;
  (void)property_data;
  (void)data_length;
  if (bytes_returned != nullptr) *bytes_returned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

auto SyncCameraSource::KsMethod(PKSMETHOD method, ULONG method_length, LPVOID method_data,
                                ULONG data_length, ULONG* bytes_returned) -> HRESULT {
  (void)method;
  (void)method_length;
  (void)method_data;
  (void)data_length;
  if (bytes_returned != nullptr) *bytes_returned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

auto SyncCameraSource::KsEvent(PKSEVENT event, ULONG event_length, LPVOID event_data,
                               ULONG data_length, ULONG* bytes_returned) -> HRESULT {
  (void)event;
  (void)event_length;
  (void)event_data;
  (void)data_length;
  if (bytes_returned != nullptr) *bytes_returned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

auto SyncCameraSource::AddRef() -> ULONG { return ++references_; }

auto SyncCameraSource::Release() -> ULONG {
  const ULONG remaining = --references_;
  if (remaining == 0) delete this;
  return remaining;
}

auto SyncCameraSource::GetEvent(DWORD flags, IMFMediaEvent** event) -> HRESULT {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    queue = events_;
  }
  return queue->GetEvent(flags, event);
}

auto SyncCameraSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->BeginGetEvent(callback, state);
}

auto SyncCameraSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->EndGetEvent(result, event);
}

auto SyncCameraSource::QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                                  const PROPVARIANT* value) -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->QueueEventParamVar(type, extended, status, value);
}

auto SyncCameraSource::GetCharacteristics(DWORD* characteristics) -> HRESULT {
  if (characteristics == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  *characteristics = MFMEDIASOURCE_IS_LIVE;
  return S_OK;
}

auto SyncCameraSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor)
    -> HRESULT {
  if (descriptor == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  if (!presentation_) return E_UNEXPECTED;
  // A clone, so a consumer selecting streams cannot mutate the source's own.
  return presentation_->Clone(descriptor);
}

auto SyncCameraSource::Start(IMFPresentationDescriptor* descriptor, const GUID* time_format,
                             const PROPVARIANT* start_position) -> HRESULT {
  if (descriptor == nullptr) return E_INVALIDARG;
  // A live source has no seeking, so the only time format it accepts is the
  // default one.
  if (time_format != nullptr && !::IsEqualGUID(*time_format, GUID_NULL)) {
    return MF_E_UNSUPPORTED_TIME_FORMAT;
  }

  ComPtr<SyncCameraStream> stream;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!stream_) return E_UNEXPECTED;
    stream = stream_;

    PROPVARIANT time;
    ::PropVariantInit(&time);
    time.vt = VT_I8;
    time.hVal.QuadPart = static_cast<LONGLONG>(SystemTime100ns());
    if (!started_) {
      started_ = true;
      events_->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK,
                                  static_cast<IMFMediaStream*>(stream_.Get()));
    } else {
      events_->QueueEventParamUnk(MEUpdatedStream, GUID_NULL, S_OK,
                                  static_cast<IMFMediaStream*>(stream_.Get()));
    }
    events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
    ::PropVariantClear(&time);
  }
  (void)start_position;
  // Outside the source lock: the stream takes its own.
  return stream->Start();
}

auto SyncCameraSource::Stop() -> HRESULT {
  ComPtr<SyncCameraStream> stream;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    stream = stream_;
    started_ = false;
    events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
  }
  return stream ? stream->Stop() : S_OK;
}

auto SyncCameraSource::Pause() -> HRESULT {
  std::lock_guard<std::mutex> guard(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  // A live camera has nothing to pause into: there is no buffered timeline to
  // resume from, so the pipeline is told plainly rather than fooled.
  return MF_E_INVALID_STATE_TRANSITION;
}

auto SyncCameraSource::Shutdown() -> HRESULT {
  ComPtr<SyncCameraStream> stream;
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    shutdown_ = true;
    stream = stream_;
    queue = events_;
  }
  if (stream) stream->Shutdown();
  if (queue) queue->Shutdown();
  return S_OK;
}

// ------------------------------------------------------------- activator ---

SyncCameraActivator::~SyncCameraActivator() = default;

auto SyncCameraActivator::Initialize() -> HRESULT {
  HRESULT hr = InitializeAttributeStore(2);
  if (FAILED(hr)) return hr;
  // The frame server reads this off the activator to identify which
  // registered source it is bringing up.
  hr = SetGUID(MFT_TRANSFORM_CLSID_Attribute, kSyncCameraSourceClsid);
  if (FAILED(hr)) return hr;
  // Deliberately not MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES: that
  // promises the pipeline a list of physical cameras this one wraps, and it
  // then fails the camera with MF_E_ATTRIBUTENOTFOUND looking for a list that
  // a purely synthetic source has no reason to have.

  source_ = new (std::nothrow) SyncCameraSource();
  if (!source_) return E_OUTOFMEMORY;
  source_->Release();  // the ComPtr is the only owner
  return source_->Initialize(this);
}

auto SyncCameraActivator::QueryInterface(REFIID riid, void** object) -> HRESULT {
  if (object == nullptr) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IMFActivate || riid == IID_IMFAttributes) {
    *object = static_cast<IMFActivate*>(this);
    AddRef();
    return S_OK;
  }
  *object = nullptr;
  return E_NOINTERFACE;
}

auto SyncCameraActivator::AddRef() -> ULONG { return ++references_; }

auto SyncCameraActivator::Release() -> ULONG {
  const ULONG remaining = --references_;
  if (remaining == 0) delete this;
  return remaining;
}

auto SyncCameraActivator::ActivateObject(REFIID riid, void** object) -> HRESULT {
  if (object == nullptr) return E_POINTER;
  *object = nullptr;
  if (!source_) return MF_E_SHUTDOWN;
  return source_->QueryInterface(riid, object);
}

auto SyncCameraActivator::ShutdownObject() -> HRESULT { return S_OK; }

auto SyncCameraActivator::DetachObject() -> HRESULT {
  source_.Reset();
  return S_OK;
}

}  // namespace noisefactor::sync::camera
